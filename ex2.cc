/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2018-20 NITK Surathkal
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Aarti Nandagiri <aarti.nandagiri@gmail.com>
 *          Vivek Jain <jain.vivek.anand@gmail.com>
 *          Mohit P. Tahiliani <tahiliani@nitk.edu.in>
 */

// This program simulates the following topology:
//
//           1000 Mbps           10Mbps          1000 Mbps
//  Sender -------------- R1 -------------- R2 -------------- Receiver
//              5ms               10ms               5ms
//
// The link between R1 and R2 is a bottleneck link with 10 Mbps. All other
// links are 1000 Mbps.
//
// This program runs by default for 100 seconds and creates a new directory
// called 'bbr-results' in the ns-3 root directory. The program creates one
// sub-directory called 'pcap' in 'bbr-results' directory (if pcap generation
// is enabled) and three .dat files.
//
// (1) 'pcap' sub-directory contains six PCAP files:
//     * bbr-0-0.pcap for the interface on Sender
//     * bbr-1-0.pcap for the interface on Receiver
//     * bbr-2-0.pcap for the first interface on R1
//     * bbr-2-1.pcap for the second interface on R1
//     * bbr-3-0.pcap for the first interface on R2
//     * bbr-3-1.pcap for the second interface on R2
// (2) cwnd.dat file contains congestion window trace for the sender node
// (3) throughput.dat file contains sender side throughput trace
// (4) queueSize.dat file contains queue length trace from the bottleneck link
//
// BBR algorithm enters PROBE_RTT phase in every 10 seconds. The congestion
// window is fixed to 4 segments in this phase with a goal to achieve a better
// estimate of minimum RTT (because queue at the bottleneck link tends to drain
// when the congestion window is reduced to 4 segments).
//
// The congestion window and queue occupancy traces output by this program show
// periodic drops every 10 seconds when BBR algorithm is in PROBE_RTT phase.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

std::string dir;
uint32_t prev = 0;
Time prevTime = Seconds (0);

// Check the queue size
void CheckQueueSize (Ptr<QueueDisc> qd)
{
  uint32_t qsize = qd->GetCurrentSize ().GetValue ();
  Simulator::Schedule (Seconds (0.2), &CheckQueueSize, qd);
  std::ofstream q (dir + "/queueSize.dat", std::ios::out | std::ios::app);
  q << Simulator::Now ().GetSeconds () << " " << qsize << std::endl;
  q.close ();
}

// Trace congestion window
static void CwndTracer (Ptr<OutputStreamWrapper> stream, uint32_t oldval, uint32_t newval)
{
  *stream->GetStream () << Simulator::Now ().GetSeconds () << " " << newval / 1448.0 << std::endl;
}

void TraceCwnd (uint32_t nodeId, uint32_t socketId)
{
  AsciiTraceHelper ascii;
  Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream (dir + "/cwnd.dat");
  Config::ConnectWithoutContext ("/NodeList/" + std::to_string (nodeId) + "/$ns3::TcpL4Protocol/SocketList/" + std::to_string (socketId) + "/CongestionWindow", MakeBoundCallback (&CwndTracer, stream));
}

// Calculate throughput
static void
TraceThroughput (Ptr<FlowMonitor> monitor)
{
  FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats ();
  auto itr = stats.begin ();
  Time curTime = Now ();
  std::ofstream thr (dir + "/ex32.dat", std::ios::out | std::ios::app);
  thr <<  curTime.GetSeconds () << " " << 8 * (itr->second.txBytes - prev) / (1000 * (curTime.GetSeconds () - prevTime.GetSeconds ())) << std::endl;
  prevTime = curTime;
  prev = itr->second.txBytes;
  Simulator::Schedule (Seconds (0.2), &TraceThroughput, monitor);
}

int main (int argc, char *argv [])
{

  uint32_t num = 1;
  uint32_t st1 = 15;
  uint32_t st2 = 0;
  std::string tcp1 = "ns3::TcpCubic";
  std::string tcp2 = "ns3::TcpBbr";
  // Naming the output directory using local system time
  time_t rawtime;
  struct tm * timeinfo;
  char buffer [80];
  time (&rawtime);
  timeinfo = localtime (&rawtime);
  strftime (buffer, sizeof (buffer), "%d-%m-%Y-%I-%M-%S", timeinfo);
  std::string currentTime (buffer);

  std::string queueDisc = "FifoQueueDisc";
  uint32_t delAckCount = 2;
  bool bql = true;
  bool enablePcap = false;
  Time stopTime = Seconds (100);

  CommandLine cmd (__FILE__);
  cmd.AddValue ("delAckCount", "Delayed ACK count", delAckCount);
  cmd.Parse (argc, argv);

  queueDisc = std::string ("ns3::") + queueDisc;

  Config::SetDefault ("ns3::TcpSocket::SndBufSize", UintegerValue (4194304));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize", UintegerValue (6291456));
  Config::SetDefault ("ns3::TcpSocket::InitialCwnd", UintegerValue (10));
  Config::SetDefault ("ns3::TcpSocket::DelAckCount", UintegerValue (delAckCount));
  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (1448));
  Config::SetDefault ("ns3::DropTailQueue<Packet>::MaxSize", QueueSizeValue (QueueSize ("1p")));
  Config::SetDefault (queueDisc + "::MaxSize", QueueSizeValue (QueueSize ("100p")));

  NodeContainer sender, receiver;
  NodeContainer routers;
  sender.Create (2);
  receiver.Create (2);
  routers.Create (2);

  // Create the point-to-point link helpers
  PointToPointHelper bottleneckLink;
  bottleneckLink.SetDeviceAttribute  ("DataRate", StringValue ("1Mbps"));
  bottleneckLink.SetChannelAttribute ("Delay", StringValue ("1ms"));

  PointToPointHelper edgeLink;
  edgeLink.SetDeviceAttribute  ("DataRate", StringValue ("1Mbps"));
  edgeLink.SetChannelAttribute ("Delay", StringValue ("1ms"));

  // Create NetDevice containers
  NetDeviceContainer senderEdge1 = edgeLink.Install (sender.Get (0), routers.Get (0));
  NetDeviceContainer senderEdge2 = edgeLink.Install (sender.Get (1), routers.Get (0));
  NetDeviceContainer r1r3 = bottleneckLink.Install (routers.Get (0), routers.Get (1));
  NetDeviceContainer receiverEdge1 = edgeLink.Install (routers.Get (1), receiver.Get (0));
  NetDeviceContainer receiverEdge2 = edgeLink.Install (routers.Get (1), receiver.Get (1));

  // Install Stack
  InternetStackHelper internet;
  internet.Install (sender);
  internet.Install (receiver);
  internet.Install (routers);

  // Configure the root queue discipline
  TrafficControlHelper tch;
  tch.SetRootQueueDisc (queueDisc);

  if (bql)
    {
      tch.SetQueueLimits ("ns3::DynamicQueueLimits", "HoldTime", StringValue ("1000ms"));
    }

  tch.Install (senderEdge1);
  tch.Install (senderEdge2);
  tch.Install (receiverEdge1);
  tch.Install (receiverEdge2);

  // Assign IP addresses
  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer i1i3 = ipv4.Assign (r1r3);

  ipv4.NewNetwork ();
  Ipv4InterfaceContainer is1 = ipv4.Assign (senderEdge1);

  ipv4.NewNetwork ();
  Ipv4InterfaceContainer is2 = ipv4.Assign (senderEdge2);

  ipv4.NewNetwork ();
  Ipv4InterfaceContainer ir1 = ipv4.Assign (receiverEdge1);

  ipv4.NewNetwork ();
  Ipv4InterfaceContainer ir2 = ipv4.Assign (receiverEdge2);

  // Populate routing tables
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // Select sender side port
  uint16_t port = 50001;

  // Install application on the sender1

  std::stringstream nodeId1; 
  nodeId1 << sender.Get(0)->GetId(); 
  std::string specificNode1 = "/NodeList/" + nodeId1.str() + "/$ns3::TcpL4Protocol/SocketType";
  TypeId tid1 = TypeId::LookupByName (tcp1);
  Config::Set (specificNode1, TypeIdValue (tid1));
  BulkSendHelper source ("ns3::TcpSocketFactory", InetSocketAddress (ir1.GetAddress (1), port));
  source.SetAttribute ("MaxBytes", UintegerValue (0));
  ApplicationContainer sourceApps = source.Install (sender.Get (0));
  sourceApps.Start (Seconds (st1));
  sourceApps.Stop (Seconds (30.0));

  // Install application on the sender2
  std::stringstream nodeId2; 
  nodeId2 << sender.Get(1)->GetId(); 
  TypeId tid2 = TypeId::LookupByName (tcp2);
  std::string specificNode2 = "/NodeList/" + nodeId2.str() + "/$ns3::TcpL4Protocol/SocketType"; 
  Config::Set (specificNode2, TypeIdValue (tid2));
  BulkSendHelper source2 ("ns3::TcpSocketFactory", InetSocketAddress (ir2.GetAddress (1), port));
  source2.SetAttribute ("MaxBytes", UintegerValue (0));
  ApplicationContainer sourceApps2 = source2.Install (sender.Get (1));
  sourceApps2.Start (Seconds (st2));
  sourceApps2.Stop (Seconds (30.0));

  // Install application on the receiver1
  PacketSinkHelper sink ("ns3::TcpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApps = sink.Install (receiver.Get (0));
  sinkApps.Start (Seconds (0.0));
  sinkApps.Stop (Seconds (30.0));

  // Install application on the receiver2
  PacketSinkHelper sink2 ("ns3::TcpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApps2 = sink2.Install (receiver.Get (1));
  sinkApps2.Start (Seconds (0.0));
  sinkApps2.Stop (Seconds (30.0));

  // Create a new directory to store the output of the program
  dir = "result1/";
  std::string dirToSave = "mkdir -p " + dir;
  if (system (dirToSave.c_str ()) == -1)
    {
      exit (1);
    }

  // The plotting scripts are provided in the following repository, if needed:
  // https://github.com/mohittahiliani/BBR-Validation/
  //
  // Download 'PlotScripts' directory (which is inside ns-3 scripts directory)
  // from the link given above and place it in the ns-3 root directory.
  // Uncomment the following three lines to generate plots for Congestion
  // Window, sender side throughput and queue occupancy on the bottleneck link.
  //
  // system (("cp -R PlotScripts/gnuplotScriptCwnd " + dir).c_str ());
  // system (("cp -R PlotScripts/gnuplotScriptThroughput " + dir).c_str ());
  // system (("cp -R PlotScripts/gnuplotScriptQueueSize " + dir).c_str ());

  // Trace the queue occupancy on the second interface of R1
  tch.Uninstall (routers.Get (0)->GetDevice (1));
  QueueDiscContainer qd;
  qd = tch.Install (routers.Get (0)->GetDevice (1));
  Simulator::ScheduleNow (&CheckQueueSize, qd.Get (0));

  // Generate PCAP traces if it is enabled
  if (enablePcap)
    {
      if (system ((dirToSave + "/pcap/").c_str ()) == -1)
        {
          exit (1);
        }
      bottleneckLink.EnablePcapAll (dir + "/pcap/bbr", true);
    }

  // Check for dropped packets using Flow Monitor
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.Install(sender.Get(num));
  Simulator::Schedule (Seconds (0.2), &TraceThroughput, monitor);

  Simulator::Stop (Seconds (30));
  Simulator::Run ();
  Simulator::Destroy ();

  return 0;
}
