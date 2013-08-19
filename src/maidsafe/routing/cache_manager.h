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

#ifndef MAIDSAFE_ROUTING_CACHE_MANAGER_H_
#define MAIDSAFE_ROUTING_CACHE_MANAGER_H_

#include <string>

#include "maidsafe/routing/api_config.h"

namespace maidsafe {

namespace routing {

namespace protobuf { class Message; }

class NetworkUtils;

class CacheManager {
 public:
  CacheManager(const NodeId& node_id, NetworkUtils& network);

  void InitialiseFunctors(MessageAndCachingFunctors message_and_caching_functors);
  void InitialiseFunctors(TypedMessageAndCachingFunctor typed_message_and_caching_functors);
  void AddToCache(const protobuf::Message& message);
  void HandleGetFromCache(protobuf::Message& message);
  bool IsInCache(const protobuf::Message& message);

 private:
  CacheManager(const CacheManager&);
  CacheManager(const CacheManager&&);
  CacheManager& operator=(const CacheManager&);

  void TypedMessageAddtoCache(const protobuf::Message& message);
  void TypedMessageHandleGetFromCache(protobuf::Message& message);
  bool TypedMessageIsInCache(const protobuf::Message& message);

  const NodeId kNodeId_;
  NetworkUtils& network_;
  MessageAndCachingFunctors message_and_caching_functors_;
  TypedMessageAndCachingFunctor typed_message_and_caching_functors_;
};

}  // namespace routing

}  // namespace maidsafe

#endif  // MAIDSAFE_ROUTING_CACHE_MANAGER_H_
