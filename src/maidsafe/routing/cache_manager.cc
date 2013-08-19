/* Copyright 2012 MaidSafe.net limited

This MaidSafe Software is licensed under the MaidSafe.net Commercial License, version 1.0 or later,
and The General Public License (GPL), version 3. By contributing code to this project You agree to
the terms laid out in the MaidSafe Contributor Agreement, version 1.0, found in the root directory
of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also available at:

http://www.novinet.com/license

Unless required by applicable law or agreed to in writing, software distributed under the License is
distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied. See the License for the specific language governing permissions and limitations under the
License.
*/

#include <future>

#include "maidsafe/routing/cache_manager.h"

#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/parameters.h"
#include "maidsafe/routing/routing.pb.h"
#include "maidsafe/routing/utils.h"


namespace maidsafe {

namespace routing {

CacheManager::CacheManager(const NodeId& node_id, NetworkUtils &network)
    : kNodeId_(node_id),
      network_(network),
      message_and_caching_functors_(),
      typed_message_and_caching_functors_() {}

void CacheManager::InitialiseFunctors(MessageAndCachingFunctors message_and_caching_functors) {
  assert(message_and_caching_functors.message_received);
  assert(message_and_caching_functors.have_cache_data);
  assert(message_and_caching_functors.store_cache_data);
  message_and_caching_functors_ = message_and_caching_functors;
}

void CacheManager::InitialiseFunctors(TypedMessageAndCachingFunctor
    typed_message_and_caching_functors) {
  assert(!message_and_caching_functors_.message_received);
  assert(!message_and_caching_functors_.have_cache_data);
  assert(!message_and_caching_functors_.store_cache_data);
  typed_message_and_caching_functors_ = typed_message_and_caching_functors;
}


void CacheManager::AddToCache(const protobuf::Message& message) {
  assert(!message.request());
  if (message_and_caching_functors_.store_cache_data) {
    message_and_caching_functors_.store_cache_data(message.data(0));
  } else {
    TypedMessageAddtoCache(message);
  }
}

void CacheManager::TypedMessageAddtoCache(const protobuf::Message& message) {
  if ((!message.has_group_source() && !message.has_group_destination()) &&
          typed_message_and_caching_functors_.single_to_single.put_cache_data) {
    typed_message_and_caching_functors_.single_to_single.put_cache_data(
        CreateSingleToSingleMessage(message));
  } else if ((!message.has_group_source() && message.has_group_destination()) &&
                typed_message_and_caching_functors_.single_to_group.put_cache_data) {
    typed_message_and_caching_functors_.single_to_group.put_cache_data(
        CreateSingleToGroupMessage(message));
  } else if ((message.has_group_source() && !message.has_group_destination()) &&
                typed_message_and_caching_functors_.group_to_single.put_cache_data) {
    typed_message_and_caching_functors_.group_to_single.put_cache_data(
        CreateGroupToSingleMessage(message));
  } else if ((message.has_group_source() && message.has_group_destination()) &&
                typed_message_and_caching_functors_.group_to_group.put_cache_data) {
    typed_message_and_caching_functors_.group_to_group.put_cache_data(
        CreateGroupToGroupMessage(message));
  }
}

// FIXME(Prakash)
bool CacheManager::HandleGetFromCache(protobuf::Message& message) {
  assert(IsRequest(message));
  assert(IsCacheableGet(message));
  auto cache_hit(std::make_shared<std::promise<bool>>());
  auto future(cache_hit->get_future());
  assert(kNodeId_.string() != message.source_id());
  if (message_and_caching_functors_.message_received) {
    if (IsRequest(message)) {
      LOG(kVerbose) << " [" << DebugId(kNodeId_) << "] rcvd : "
                    << MessageTypeString(message) << " from "
                    << HexSubstr(message.source_id())
                    << "   (id: " << message.id() << ")  --NodeLevel-- caching";
      ReplyFunctor response_functor = [=](const std::string& reply_message) {
          if (reply_message.empty()) {
            LOG(kVerbose) << "No cache available, passing on the original request";
            //return network_.SendToClosestNode(message);
            cache_hit->set_value(false);
          }

          cache_hit->set_value(true);
          //  Responding with cached response
          protobuf::Message message_out;
          message_out.set_request(false);
          message_out.set_hops_to_live(Parameters::hops_to_live);
          message_out.set_destination_id(message.source_id());
          message_out.set_type(message.type());
          message_out.set_direct(true);
          message_out.clear_data();
          message_out.set_client_node(message.client_node());
          message_out.set_routing_message(message.routing_message());
          message_out.add_data(reply_message);
          message_out.set_last_id(kNodeId_.string());
          message_out.set_source_id(kNodeId_.string());
          if (message.has_cacheable())
            message_out.set_cacheable(message.cacheable());
          if (message.has_id())
            message_out.set_id(message.id());
          else
            LOG(kInfo) << "Message to be sent back had no ID.";

          if (message.has_relay_id())
            message_out.set_relay_id(message.relay_id());

          if (message.has_relay_connection_id()) {
            message_out.set_relay_connection_id(message.relay_connection_id());
          }
          network_.SendToClosestNode(message_out);
      };

      if (message_and_caching_functors_.message_received)
        message_and_caching_functors_.message_received(message.data(0), true, response_functor);
    }
  } else {
    return TypedMessageHandleGetFromCache(message);
  }
  return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
}

bool CacheManager::TypedMessageHandleGetFromCache(protobuf::Message& message) {
  if ((!message.has_group_source() && !message.has_group_destination()) &&
      typed_message_and_caching_functors_.single_to_single.get_cache_data) {
     return typed_message_and_caching_functors_.single_to_single.get_cache_data(
                CreateSingleToSingleMessage(message));
  } else if ((!message.has_group_source() && message.has_group_destination()) &&
              typed_message_and_caching_functors_.single_to_group.get_cache_data) {
      return typed_message_and_caching_functors_.single_to_group.get_cache_data(
                 CreateSingleToGroupMessage(message));
  } else if ((message.has_group_source() && !message.has_group_destination()) &&
                typed_message_and_caching_functors_.group_to_single.get_cache_data) {
      return typed_message_and_caching_functors_.group_to_single.get_cache_data(
                 CreateGroupToSingleMessage(message));
  } else if ((message.has_group_source() && message.has_group_destination()) &&
                typed_message_and_caching_functors_.group_to_group.get_cache_data) {
      return typed_message_and_caching_functors_.group_to_group.get_cache_data(
                 CreateGroupToGroupMessage(message));
  }
  return false;
}

}  // namespace routing

}  // namespace maidsafe
