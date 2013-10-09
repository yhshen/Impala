// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <thrift/protocol/TDebugProtocol.h>

#include "catalog/catalog-server.h"
#include "catalog/catalog-util.h"
#include "statestore/state-store-subscriber.h"
#include "util/debug-util.h"
#include "gen-cpp/CatalogObjects_types.h"
#include "gen-cpp/CatalogService_types.h"

using namespace impala;
using namespace std;
using namespace boost;
using namespace apache::thrift;

DEFINE_int32(catalog_service_port, 26000, "port where the CatalogService is running");
DECLARE_string(state_store_host);
DECLARE_int32(state_store_subscriber_port);
DECLARE_int32(state_store_port);
DECLARE_string(hostname);

string CatalogServer::IMPALA_CATALOG_TOPIC = "catalog-update";

// Implementation for the CatalogService thrift interface.
class CatalogServiceThriftIf : public CatalogServiceIf {
 public:
  CatalogServiceThriftIf(CatalogServer* catalog_server)
      : catalog_server_(catalog_server) {
  }

  // Executes a TDdlExecRequest and returns details on the result of the operation.
  virtual void ExecDdl(TDdlExecResponse& resp, const TDdlExecRequest& req) {
    VLOG_RPC << "ExecDdl(): request=" << ThriftDebugString(req);
    Status status = catalog_server_->catalog()->ExecDdl(req, &resp);
    if (!status.ok()) LOG(ERROR) << status.GetErrorMsg();
    TStatus thrift_status;
    status.ToThrift(&thrift_status);
    resp.result.__set_status(thrift_status);
    VLOG_RPC << "ExecDdl(): response=" << ThriftDebugString(resp);
  }

  // Executes a TResetMetadataRequest and returns details on the result of the operation.
  virtual void ResetMetadata(TResetMetadataResponse& resp,
      const TResetMetadataRequest& req) {
    VLOG_RPC << "ResetMetadata(): request=" << ThriftDebugString(req);
    Status status = catalog_server_->catalog()->ResetMetadata(req, &resp);
    if (!status.ok()) LOG(ERROR) << status.GetErrorMsg();
    TStatus thrift_status;
    status.ToThrift(&thrift_status);
    resp.result.__set_status(thrift_status);
    VLOG_RPC << "ResetMetadata(): response=" << ThriftDebugString(resp);
  }

  // Executes a TUpdateMetastoreRequest and returns details on the result of the
  // operation.
  virtual void UpdateMetastore(TUpdateMetastoreResponse& resp,
      const TUpdateMetastoreRequest& req) {
    VLOG_RPC << "UpdateMetastore(): request=" << ThriftDebugString(req);
    Status status = catalog_server_->catalog()->UpdateMetastore(req, &resp);
    if (!status.ok()) LOG(ERROR) << status.GetErrorMsg();
    TStatus thrift_status;
    status.ToThrift(&thrift_status);
    resp.result.__set_status(thrift_status);
    VLOG_RPC << "UpdateMetastore(): response=" << ThriftDebugString(resp);
  }

 private:
  CatalogServer* catalog_server_;
};

CatalogServer::CatalogServer(Metrics* metrics)
  : thrift_iface_(new CatalogServiceThriftIf(this)),
    metrics_(metrics),
    last_catalog_version_(0L) {
}

Status CatalogServer::Start() {
  TNetworkAddress subscriber_address =
      MakeNetworkAddress(FLAGS_hostname, FLAGS_state_store_subscriber_port);
  TNetworkAddress statestore_address =
      MakeNetworkAddress(FLAGS_state_store_host, FLAGS_state_store_port);
  TNetworkAddress server_address = MakeNetworkAddress(FLAGS_hostname,
      FLAGS_catalog_service_port);

  stringstream subscriber_id;
  subscriber_id << server_address;

  // This will trigger a full Catalog metadata load.
  catalog_.reset(new Catalog());

  state_store_subscriber_.reset(new StateStoreSubscriber(subscriber_id.str(),
     subscriber_address, statestore_address, metrics_));

  StateStoreSubscriber::UpdateCallback cb =
      bind<void>(mem_fn(&CatalogServer::UpdateCatalogTopicCallback), this, _1, _2);
  Status status = state_store_subscriber_->AddTopic(IMPALA_CATALOG_TOPIC, false, cb);
  if (!status.ok()) {
    status.AddErrorMsg("CatalogService failed to start");
    return status;
  }
  RETURN_IF_ERROR(state_store_subscriber_->Start());
  return Status::OK;
}

void CatalogServer::RegisterWebpages(Webserver* webserver) {
  Webserver::PathHandlerCallback catalog_callback =
      bind<void>(mem_fn(&CatalogServer::CatalogPathHandler), this, _1, _2);
  webserver->RegisterPathHandler("/catalog", catalog_callback);

  Webserver::PathHandlerCallback catalog_objects_callback =
      bind<void>(mem_fn(&CatalogServer::CatalogObjectsPathHandler), this, _1, _2);
  webserver->RegisterPathHandler("/catalog_objects",
      catalog_objects_callback, false, false);
}

void CatalogServer::UpdateCatalogTopicCallback(
    const StateStoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
    vector<TTopicDelta>* subscriber_topic_updates) {
  StateStoreSubscriber::TopicDeltaMap::const_iterator topic =
      incoming_topic_deltas.find(CatalogServer::IMPALA_CATALOG_TOPIC);
  if (topic == incoming_topic_deltas.end()) return;

  // This function determines what items have been added/removed from the catalog
  // since the last heartbeat. To do this, it gets all the catalog objects from
  // JniCatalog and enumerates all these objects, looking for the objects that
  // have a catalog version that is > the max() catalog version sent with the
  // last heartbeat. To determine items that have been deleted, we save the set of
  // topic entry keys sent with the last update and look at the difference between it
  // and the current set of topic entry keys.
  // The key for each entry is a string composed of:
  // "TCatalogObjectType:<unique object name>". So for table foo.bar, the key would be
  // "TABLE:foo.bar". By encoding the object type information in the key it helps uniquify
  // the keys as well as help to determine what object type was removed in a state store
  // delta update since the state store only sends key names for deleted items.
  const TTopicDelta& delta = topic->second;
  // If this is not a delta update, add all known catalog objects to the topic.
  if (!delta.is_delta) {
    catalog_object_topic_entry_keys_.clear();
    last_catalog_version_ = 0;
  }

  // First, call into the Catalog to get all the catalog objects (as Thrift structs).
  TGetAllCatalogObjectsRequest req;
  req.__set_from_version(last_catalog_version_);
  TGetAllCatalogObjectsResponse resp;
  Status s = catalog_->GetAllCatalogObjects(req, &resp);
  if (!s.ok()) {
    LOG(ERROR) << s.GetErrorMsg();
    return;
  }
  LOG_EVERY_N(INFO, 300) << "Catalog Version: " << resp.max_catalog_version << " "
                         << "Last Catalog Version: " << last_catalog_version_;
  set<string> current_entry_keys;

  // Add any new/updated catalog objects to the topic.
  BOOST_FOREACH(const TCatalogObject& catalog_object, resp.objects) {
    const string& entry_key = TCatalogObjectToEntryKey(catalog_object);
    if (entry_key.empty()) {
      LOG_EVERY_N(WARNING, 60) << "Unable to build topic entry key for TCatalogObject: "
                               << ThriftDebugString(catalog_object);
    }
    current_entry_keys.insert(entry_key);

    // Check if we knew about this topic entry key in the last update, and if so remove it
    // from the catalog_object_topic_entry_keys_. At the end of this loop, we will be left
    // with the set of keys that were in the last update, but not in this update,
    // indicating which objects have been removed/dropped.
    set<string>::iterator itr = catalog_object_topic_entry_keys_.find(entry_key);
    if (itr != catalog_object_topic_entry_keys_.end()) {
      catalog_object_topic_entry_keys_.erase(itr);
    }

    // This isn't a new item, skip it.
    if (catalog_object.catalog_version <= last_catalog_version_) continue;

    VLOG(1) << "Adding Update: " << entry_key << "@"
              << catalog_object.catalog_version;

    subscriber_topic_updates->push_back(TTopicDelta());
    TTopicDelta& update = subscriber_topic_updates->back();
    update.topic_name = IMPALA_CATALOG_TOPIC;

    update.topic_entries.push_back(TTopicItem());
    TTopicItem& item = update.topic_entries.back();
    item.key = entry_key;

    ThriftSerializer thrift_serializer(false);
    Status status = thrift_serializer.Serialize(&catalog_object, &item.value);
    if (!status.ok()) {
      LOG(ERROR) << "Error serializing topic value: " << status.GetErrorMsg();
      subscriber_topic_updates->pop_back();
    }
  }

  // Add all deleted items to the topic. Any remaining items in
  // catalog_object_topic_entry_keys_ indicate that the object was dropped since the
  // last update, so mark it as deleted.
  BOOST_FOREACH(const string& key, catalog_object_topic_entry_keys_) {
    subscriber_topic_updates->push_back(TTopicDelta());
    TTopicDelta& update = subscriber_topic_updates->back();
    update.topic_name = IMPALA_CATALOG_TOPIC;
    update.topic_entries.push_back(TTopicItem());
    TTopicItem& item = update.topic_entries.back();
    item.key = key;
    VLOG(1) << "Adding deletion: " << key;
    // Don't set a value to mark this item as deleted.
  }

  // Update the new catalog version and the set of known catalog objects.
  catalog_object_topic_entry_keys_.swap(current_entry_keys);
  last_catalog_version_ = resp.max_catalog_version;
}

// TODO: Create utility function for rendering the Catalog handler so it can
// be shared between CatalogServer and ImpalaServer
void CatalogServer::CatalogPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TGetDbsResult get_dbs_result;
  Status status = catalog_->GetDbNames(NULL, &get_dbs_result);
  if (!status.ok()) {
    (*output) << "Error: " << status.GetErrorMsg();
    return;
  }
  vector<string>& db_names = get_dbs_result.dbs;

  if (args.find("raw") == args.end()) {
    (*output) << "<h2>Catalog</h2>" << endl;

    // Build a navigation string like [ default | tpch | ... ]
    vector<string> links;
    BOOST_FOREACH(const string& db, db_names) {
      stringstream ss;
      ss << "<a href='#" << db << "'>" << db << "</a>";
      links.push_back(ss.str());
    }
    (*output) << "[ " <<  join(links, " | ") << " ] ";

    BOOST_FOREACH(const string& db, db_names) {
      (*output) << "<a id='" << db << "'><h3>" << db << "</h3></a>";
      TGetTablesResult get_table_results;
      Status status = catalog_->GetTableNames(db, NULL, &get_table_results);
      if (!status.ok()) {
        (*output) << "Error: " << status.GetErrorMsg();
        continue;
      }
      vector<string>& table_names = get_table_results.tables;
      (*output) << "<p>" << db << " contains <b>" << table_names.size()
                << "</b> tables</p>";

      (*output) << "<ul>" << endl;
      BOOST_FOREACH(const string& table, table_names) {
        (*output) << "<li>" << table << "</li>" << endl;
      }
      (*output) << "</ul>" << endl;
    }
  } else {
    (*output) << "Catalog" << endl << endl;
    (*output) << "List of databases:" << endl;
    (*output) << join(db_names, "\n") << endl << endl;

    BOOST_FOREACH(const string& db, db_names) {
      TGetTablesResult get_table_results;
      Status status = catalog_->GetTableNames(db, NULL, &get_table_results);
      if (!status.ok()) {
        (*output) << "Error: " << status.GetErrorMsg();
        continue;
      }
      vector<string>& table_names = get_table_results.tables;
      (*output) << db << " contains " << table_names.size()
                << " tables" << endl;
      BOOST_FOREACH(const string& table, table_names) {
        (*output) << "- " << table << endl;
      }
      (*output) << endl << endl;
    }
  }
}

void CatalogServer::CatalogObjectsPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  Webserver::ArgumentMap::const_iterator object_type_arg = args.find("object_type");
  Webserver::ArgumentMap::const_iterator object_name_arg = args.find("object_name");
  if (object_type_arg != args.end() && object_name_arg != args.end()) {
    TCatalogObjectType::type object_type =
        TCatalogObjectTypeFromName(object_type_arg->second);

    // Get the object type and name from the topic entry key
    TCatalogObject request;
    TCatalogObjectFromObjectName(object_type, object_name_arg->second, &request);

    // Get the object and dump its contents.
    TCatalogObject result;
    Status status = catalog_->GetCatalogObject(request, &result);
    if (status.ok()) {
      if (args.find("raw") == args.end()) {
        (*output) << "<pre>" << ThriftDebugString(result) << "</pre>";
      } else {
        (*output) << ThriftDebugString(result);
      }
    } else {
      (*output) << status.GetErrorMsg();
    }
  } else {
    (*output) << "Please specify values for the object_type and object_name parameters.";
  }
}
