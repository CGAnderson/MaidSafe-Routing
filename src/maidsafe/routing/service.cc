/*******************************************************************************
 *  Copyright 2012 maidsafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of maidsafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of maidsafe.net. *
 ******************************************************************************/

#include "maidsafe/routing/service.h"

#include <string>
#include <vector>

#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/rudp/managed_connections.h"
#include "maidsafe/rudp/return_codes.h"

#include "maidsafe/routing/message_handler.h"
#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/node_id.h"
#include "maidsafe/routing/non_routing_table.h"
#include "maidsafe/routing/parameters.h"
#include "maidsafe/routing/routing_pb.h"
#include "maidsafe/routing/routing_table.h"
#include "maidsafe/routing/utils.h"


namespace maidsafe {

namespace routing {

namespace {

typedef boost::asio::ip::udp::endpoint Endpoint;

}  // unnamed namespace

namespace service {

void Ping(RoutingTable& routing_table, protobuf::Message& message) {
  if (message.destination_id() != routing_table.kKeys().identity) {
    // Message not for this node and we should not pass it on.
    LOG(kError) << "Message not for this node.";
    message.Clear();
    return;
  }
  protobuf::PingResponse ping_response;
  protobuf::PingRequest ping_request;

  if (!ping_request.ParseFromString(message.data(0))) {
    LOG(kError) << "No Data.";
    return;
  }
  ping_response.set_pong(true);
  ping_response.set_original_request(message.data(0));
  ping_response.set_original_signature(message.signature());
  ping_response.set_timestamp(GetTimeStamp());
  message.set_request(false);
  message.clear_route_history();
  message.clear_data();
  message.add_data(ping_response.SerializeAsString());
  message.set_destination_id(message.source_id());
  message.set_source_id(routing_table.kKeys().identity);
  message.set_hops_to_live(Parameters::hops_to_live);
  assert(message.IsInitialized() && "unintialised message");
}

void Connect(RoutingTable& routing_table,
             NonRoutingTable& non_routing_table,
             NetworkUtils& network,
             protobuf::Message& message,
             RequestPublicKeyFunctor request_public_key_functor) {
  if (message.destination_id() != routing_table.kKeys().identity) {
    // Message not for this node and we should not pass it on.
    LOG(kError) << "Message not for this node.";
    message.Clear();
    return;
  }
  protobuf::ConnectRequest connect_request;
  protobuf::ConnectResponse connect_response;
  if (!connect_request.ParseFromString(message.data(0))) {
    LOG(kVerbose) << "Unable to parse connect request.";
    message.Clear();
    return;
  }
  NodeInfo node;
  node.node_id = NodeId(connect_request.contact().node_id());
  LOG(kVerbose) <<"[" << HexSubstr(routing_table.kKeys().identity) << "]"
                << " received Connect request from "
                << HexSubstr(connect_request.contact().node_id());
  connect_response.set_answer(false);
  rudp::EndpointPair this_endpoint_pair1, this_endpoint_pair2, peer_endpoint_pair;
  peer_endpoint_pair.external =
      GetEndpointFromProtobuf(connect_request.contact().public_endpoint());
  peer_endpoint_pair.local = GetEndpointFromProtobuf(connect_request.contact().private_endpoint());

  rudp::NatType nat_type = NatTypeFromProtobuf(connect_request.contact().nat_type());
  rudp::NatType this_nat_type(rudp::NatType::kUnknown);
  if (!peer_endpoint_pair.external.address().is_unspecified()) {
    if (network.GetAvailableEndpoint(peer_endpoint_pair.external, this_endpoint_pair1,
                                     this_nat_type) != rudp::kSuccess) {
      LOG(kWarning) << "Unable to get available endpoint to connect to "
                    << peer_endpoint_pair.external;
    }
  }

  if (!peer_endpoint_pair.local.address().is_unspecified() &&
      (peer_endpoint_pair.local != peer_endpoint_pair.external)) {
    if (network.GetAvailableEndpoint(peer_endpoint_pair.local, this_endpoint_pair2,
                                     this_nat_type) != rudp::kSuccess) {
      LOG(kWarning) << "Unable to get available endpoint to connect to "
                    << peer_endpoint_pair.local;
    }
  }

  rudp::EndpointPair this_endpoint_pair;
  this_endpoint_pair.external = this_endpoint_pair1.external;
  this_endpoint_pair.local = this_endpoint_pair2.local;

  if (this_endpoint_pair.external.address().is_unspecified() &&
      this_endpoint_pair.local.address().is_unspecified()) {
    LOG(kError) << "Unable to get any available endpoint to connect to ";
    return;
  }

  NodeId peer_node_id(connect_request.contact().node_id());
// Handling the case when this node and peer node are behind symmetric router
  if ((nat_type == rudp::NatType::kSymmetric) && (this_nat_type == rudp::NatType::kSymmetric)) {
//    peer_endpoint_pair.external = Endpoint();  // No need to try connction on external endpoint.
    auto validate_node = [=, &routing_table] (const asymm::PublicKey& key)->void {
        LOG(kInfo) << "validation callback called with public key for" << DebugId(peer_node_id)
                   << " -- pseudo connection";
        HandleSymmetricNodeAdd(routing_table, peer_node_id, key);
      };

    TaskResponseFunctor add_symmetric_node =
      [=, &routing_table, &request_public_key_functor](std::vector<std::string>) {
        if (request_public_key_functor)
          request_public_key_functor(peer_node_id, validate_node);
      };
    network.timer().AddTask(boost::posix_time::seconds(5), add_symmetric_node, 1);
  }

  bool check_node_succeeded(false);
  if (message.client_node()) {  // Client node, check non-routing table
    LOG(kVerbose) << "Client connect request - will check non-routing table.";
    NodeId furthest_close_node_id =
        routing_table.GetNthClosestNode(NodeId(routing_table.kKeys().identity),
                                        Parameters::closest_nodes_size).node_id;
    check_node_succeeded = non_routing_table.CheckNode(node, furthest_close_node_id);
  } else {
    LOG(kVerbose) << "Server connect request - will check routing table.";
    check_node_succeeded = routing_table.CheckNode(node);
  }

  if (check_node_succeeded) {
    LOG(kVerbose) << "CheckNode(node) for " << (message.client_node() ? "client" : "server")
                  << " node succeeded.";
    if (request_public_key_functor) {
      auto validate_node =
          [=, &routing_table, &non_routing_table, &network] (const asymm::PublicKey& key)->void {
            LOG(kInfo) << "validation callback called with public key for" << DebugId(peer_node_id);
            ValidateAndAddToRudp(network,
                                 NodeId(routing_table.kKeys().identity),
                                 peer_node_id,
                                 key,
                                 peer_endpoint_pair,
                                 this_endpoint_pair,
                                 routing_table.client_mode());
          };
      request_public_key_functor(peer_node_id, validate_node);
      connect_response.set_answer(true);
      connect_response.mutable_contact()->set_node_id(routing_table.kKeys().identity);
      connect_response.mutable_contact()->set_nat_type(NatTypeProtobuf(this_nat_type));
      SetProtobufEndpoint(this_endpoint_pair.local,
                          connect_response.mutable_contact()->mutable_private_endpoint());
      SetProtobufEndpoint(this_endpoint_pair.external,
                          connect_response.mutable_contact()->mutable_public_endpoint());
    }
  }

  connect_response.set_timestamp(GetTimeStamp());
  connect_response.set_original_request(message.data(0));
  connect_response.set_original_signature(message.signature());
  NodeId source((message.has_relay() ? message.relay_id() : message.source_id()));
  if (connect_request.closest_id_size() > 0) {
    for (auto node_id : routing_table.GetClosestNodes(source,
                                                      Parameters::max_routing_table_size)) {
      if (std::find(connect_request.closest_id().begin(), connect_request.closest_id().end(),
                    node_id.String()) == connect_request.closest_id().end() &&
          (NodeId::CloserToTarget(node_id,
              NodeId(connect_request.closest_id(connect_request.closest_id_size() - 1)), source) ||
           (connect_request.closest_id_size() + connect_response.closer_id_size() <
            Parameters::max_routing_table_size)) &&
          source != node_id)
        connect_response.add_closer_id(node_id.String());
    }
  }
  message.clear_route_history();
  message.clear_data();
  message.add_data(connect_response.SerializeAsString());
  message.set_direct(true);
  message.set_replication(1);
  message.set_client_node(routing_table.client_mode());
  message.set_request(false);
  message.set_hops_to_live(Parameters::hops_to_live);
  if (message.has_source_id())
    message.set_destination_id(message.source_id());
  else
    message.clear_destination_id();
  message.set_source_id(routing_table.kKeys().identity);
  assert(message.IsInitialized() && "unintialised message");
}

void FindNodes(RoutingTable& routing_table, protobuf::Message& message) {
  protobuf::FindNodesRequest find_nodes;
  if (!find_nodes.ParseFromString(message.data(0))) {
    LOG(kWarning) << "Unable to parse find node request.";
    message.Clear();
    return;
  }
  if (0 == find_nodes.num_nodes_requested()) {
    LOG(kWarning) << "Invalid find node request.";
    message.Clear();
    return;
  }

  LOG(kVerbose) << "[" << HexSubstr(routing_table.kKeys().identity) << "]"
                << " parsed find node request for : " << HexSubstr(find_nodes.target_node());
  protobuf::FindNodesResponse found_nodes;
  std::vector<NodeId> nodes(routing_table.GetClosestNodes(NodeId(find_nodes.target_node()),
                              static_cast<uint16_t>(find_nodes.num_nodes_requested() - 1)));

  found_nodes.add_nodes(routing_table.kKeys().identity);

  for (auto node : nodes)
    found_nodes.add_nodes(node.String());

  LOG(kVerbose) << "Responding Find node with " << found_nodes.nodes_size()  << " contacts.";

  found_nodes.set_original_request(message.data(0));
  found_nodes.set_original_signature(message.signature());
  found_nodes.set_timestamp(GetTimeStamp());
  assert(found_nodes.IsInitialized() && "unintialised found_nodes response");
  if (message.has_source_id()) {
    message.set_destination_id(message.source_id());
  } else {
    message.clear_destination_id();
    LOG(kVerbose) << "Relay message, so not setting destination ID.";
  }
  message.set_source_id(routing_table.kKeys().identity);
  message.clear_route_history();
  message.clear_data();
  message.add_data(found_nodes.SerializeAsString());
  message.set_direct(true);
  message.set_replication(1);
  message.set_client_node(routing_table.client_mode());
  message.set_request(false);
  message.set_hops_to_live(Parameters::hops_to_live);
  assert(message.IsInitialized() && "unintialised message");
}

void ProxyConnect(RoutingTable& routing_table,
                  NetworkUtils& /*network*/,
                  protobuf::Message& message) {
  if (message.destination_id() != routing_table.kKeys().identity) {
    // Message not for this node and we should not pass it on.
    LOG(kError) << "Message not for this node.";
    message.Clear();
    return;
  }
  protobuf::ProxyConnectResponse proxy_connect_response;
  protobuf::ProxyConnectRequest proxy_connect_request;

  if (!proxy_connect_request.ParseFromString(message.data(0))) {
    LOG(kError) << "No Data";
    message.Clear();
    return;
  }

  // TODO(Prakash): any validation needed?
  rudp::EndpointPair endpoint_pair;
  endpoint_pair.external = GetEndpointFromProtobuf(proxy_connect_request.external_endpoint());
  endpoint_pair.local = GetEndpointFromProtobuf(proxy_connect_request.local_endpoint());

  // TODO(Prakash): Also check NRT and if its my bootstrap endpoint.
  if (routing_table.IsConnected(endpoint_pair.external)) {
    // If already in routing table
    proxy_connect_response.set_result(protobuf::kAlreadyConnected);
  } else {
    bool connect_result(false);
    // TODO(Prakash):  connect_result = rudp.TryConnect(endpoint);
    if (connect_result)
      proxy_connect_response.set_result(protobuf::kSuccess);
    else
      proxy_connect_response.set_result(protobuf::kFailure);
  }
  message.set_request(false);
  message.clear_route_history();
  message.clear_data();
  message.add_data(proxy_connect_response.SerializeAsString());
  message.set_direct(true);
  message.set_destination_id(message.source_id());
  message.set_source_id(routing_table.kKeys().identity);
  message.set_hops_to_live(Parameters::hops_to_live);
  message.set_client_node(routing_table.client_mode());
  assert(message.IsInitialized() && "unintialised message");
}

}  // namespace service

}  // namespace routing

}  // namespace maidsafe
