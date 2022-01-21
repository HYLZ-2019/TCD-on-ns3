/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2019 NITK Surathkal
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
 * Authors: Apoorva Bhargava <apoorvabhargava13@gmail.com>
 */

// Network topology
//
//       n0 ---------- n1 ---------- n2 ---------- n3
//            10 Mbps       1 Mbps        10 Mbps
//             1 ms         10 ms          1 ms
//
// - TCP flow from n0 to n3 using BulkSendApplication.
// - The following simulation output is stored in results/ in ns-3 top-level directory:
//   - cwnd traces are stored in cwndTraces folder
//   - queue length statistics are stored in queue-size.dat file
//   - pcaps are stored in pcap folder
//   - queueTraces folder contain the drop statistics at queue
//   - queueStats.txt file contains the queue stats and config.txt file contains
//     the simulation configuration.
// - The cwnd and queue length traces obtained from this example were tested against
//   the respective traces obtained from Linux Reno by using ns-3 Direct Code Execution.
//   See internet/doc/tcp.rst for more details.

#include <iostream>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include <bits/stdc++.h>

using namespace ns3;
std::string dir = "results/";
Time stopTime = Seconds (60);
uint32_t segmentSize = 524;




//这东西是一个跟踪输出cwnd变化的，现在不要tcp就也不要cwnd
// Function to trace change in cwnd at n0
/*
static void
CwndChange (uint32_t oldCwnd, uint32_t newCwnd)
{
  std::ofstream fPlotQueue (dir + "cwndTraces/n0.dat", std::ios::out | std::ios::app);
  fPlotQueue << Simulator::Now ().GetSeconds () << " " << newCwnd / segmentSize << std::endl;
  fPlotQueue.close ();
}*/

// Function to calculate drops in a particular Queue
static void
DropAtQueue (Ptr<OutputStreamWrapper> stream, Ptr<const QueueDiscItem> item)
{
  *stream->GetStream () << Simulator::Now ().GetSeconds () << " 1" << std::endl;
}

// Trace Function for cwnd
/*
void
TraceCwnd (uint32_t node, uint32_t cwndWindow,
           Callback <void, uint32_t, uint32_t> CwndTrace)
{
  Config::ConnectWithoutContext ("/NodeList/" + std::to_string (node) + "/$ns3::TcpL4Protocol/SocketList/" + std::to_string (cwndWindow) + "/CongestionWindow", CwndTrace);
}*/

// Function to install BulkSend application
/*void InstallBulkSend (Ptr<Node> node, Ipv4Address address, uint16_t port, std::string socketFactory,
                      uint32_t nodeId, uint32_t cwndWindow,
                      Callback <void, uint32_t, uint32_t> CwndTrace)*/

void InstallBulkSend (Ptr<Node> node, Ipv4Address address, uint16_t port, std::string socketFactory) //TCP only
{
  BulkSendHelper source (socketFactory, InetSocketAddress (address, port));
  source.SetAttribute ("MaxBytes", UintegerValue (0)); //不知道这个参数是干嘛的
  ApplicationContainer sourceApps = source.Install (node);
  sourceApps.Start (Seconds (10.0));
  //Simulator::Schedule (Seconds (10.0) + Seconds (0.001), &TraceCwnd, nodeId, cwndWindow, CwndTrace);
  //把定时跟踪输出去掉
  sourceApps.Stop (stopTime);
}

void InstallOnOffSend (Ptr<Node> node, Ipv4Address address, uint16_t port, std::string socketFactory, 
                      std::string onTime, std::string offTime, uint64_t payloadSize, std::string dataRate)
{
  OnOffHelper onoff (socketFactory, InetSocketAddress (address, port));
  onoff.SetAttribute ("OnTime",  StringValue(onTime));
  onoff.SetAttribute ("OffTime", StringValue(offTime));
  onoff.SetAttribute ("PacketSize", UintegerValue(payloadSize));
  onoff.SetAttribute ("DataRate", StringValue(dataRate)); //bit/s
  ApplicationContainer onoffApps = onoff.Install (node);
  onoffApps.Start (Seconds (1.0));
  onoffApps.Stop (stopTime);

  //Simulator::Schedule (Seconds (10.0) + Seconds (0.001), &TraceCwnd, nodeId, cwndWindow, CwndTrace);
  //把定时跟踪输出去掉
}


// Function to install sink application
void InstallPacketSink (Ptr<Node> node, uint16_t port, std::string socketFactory)
{
  PacketSinkHelper sink (socketFactory, InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApps = sink.Install (node);
  sinkApps.Start (Seconds (1.0));
  sinkApps.Stop (stopTime);
}

void InstallUdpSever(Ptr<Node> node, uint16_t port) 
{
  UdpServerHelper server (port);
  ApplicationContainer apps = server.Install (node);
  apps.Start (Seconds (1.0));
  apps.Stop (stopTime);
}

LosslessOnoffTable* globalOnoffTable;

int n, m;
NodeContainer nodes;
std::vector <NetDeviceContainer> netDevices;
std::vector <PointToPointHelper> channelHelpers;
std::vector <Ipv4InterfaceContainer> IPAddresses;
InternetStackHelper internetStack;
TrafficControlHelper tch;
QueueDiscContainer qd;
std::vector <std::string> ipBase;
std::vector <std::string> ipMask;

std::string qdiscTypeId = "ns3::FifoQueueDisc"; 
//这个是装在TC Layer上的队列type, 现在把它改成on-off model的，

//我们要新造一个qdiscType，它需要用另一种和routing包平行的包来和相邻的router交流堵塞信息，并据此更新自己的路由表。
//特别地，我们是lossless network，所以不能Drop包，只能Congest包。这个应该有封装得比较好的函数可以用来做。



// Function to check queue length of all queues and save results in dir + "queue-size-num.dat"
// Why "queue" is passed in and not used is because I don't want to understand the APIs of Schedule.
void
CheckQueueSize (Ptr<QueueDisc> queue)
{
  int queue_num = 0;
  for (int i = 0; i < n; ++i) {
    Ptr<ns3::Node> node = nodes.Get(i);
    uint32_t num = node -> GetNDevices();
    for (uint32_t k = 0; k + 1 != num; ++k) {
      Ptr<NetDevice> dev = node -> GetDevice (k);
      // Output queue statics for every queue (~every device)
      queue = qd.Get(queue_num);
      uint32_t qSize = queue->GetCurrentSize ().GetValue ();
      std::ofstream fPlotQueue (std::stringstream (dir + "queue-" + std::to_string(i) + "-" + std::to_string(k) + ".dat").str ().c_str (), std::ios::out | std::ios::app);
      fPlotQueue << Simulator::Now ().GetSeconds () << " " << qSize <<" ";
      bool on = queue->onoffTable->getValue(dev->GetAddress());
      if (on){
        fPlotQueue << "[ON]\n";
      }
      else{
        fPlotQueue << "[OFF]\n";
      }
      fPlotQueue.close ();
      queue_num ++;
    }  // Check queue size every 1/100 of a second
  }
  Simulator::Schedule (Seconds (1), &CheckQueueSize, queue);
}

void build(std::string filename) {
  std::freopen(filename.c_str(), "r", stdin);
  std::cin >> n >> m;
  // Create nodes
  nodes.Create(n);

  // Build Channels
  for (int i = 0; i < m; ++i) {
    int x, y;
    std::cin >> x >> y;
    std::string DataRate, Delay, Address, Mask;
    std::cin >> DataRate >> Delay >> Address >> Mask;
    // Create the point-to-point link helpers and connect two nodes
    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute    ("DataRate", StringValue (DataRate));
    p2ph.SetChannelAttribute   ("Delay", StringValue (Delay));
    netDevices.push_back(p2ph.Install(nodes.Get(x), nodes.Get(y)));
    channelHelpers.push_back(p2ph);

    ipBase.push_back(Address);
    ipMask.push_back(Mask);
  }

  std::fclose(stdin);
  internetStack.Install (nodes); // Add one more device to each node

  // Assign IP addresses to all the network devices
  for (int i = 0; i < m; ++i) {
    Ipv4AddressHelper ipah (ipBase[i].c_str(), ipMask[i].c_str());
    IPAddresses.push_back(ipah.Assign (netDevices[i]));
  }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // Set default parameters for queue discipline
  Config::SetDefault (qdiscTypeId + "::MaxSize", QueueSizeValue (QueueSize ("1000p")));

  // Install queue discipline on router
  tch.SetRootQueueDisc (qdiscTypeId);
  for (int i = 0; i < n; ++i) {
    Ptr<ns3::Node> node = nodes.Get(i);
    uint32_t num = node -> GetNDevices();
    for (uint32_t k = 0; k + 1 != num; ++k) {
      Ptr<NetDevice> dev = node -> GetDevice (k);
      tch.Uninstall (dev);
      qd.Add (tch.Install (dev, globalOnoffTable).Get (0));
    }
  }

  // Enable BQL
  tch.SetQueueLimits ("ns3::DynamicQueueLimits");
}

int main (int argc, char *argv[])
{
  globalOnoffTable = new LosslessOnoffTable();
  globalOnoffTable->globalInit();

  uint32_t stream = 1;
  std::string transportProt = "Udp";
  std::string socketFactory = "ns3::UdpSocketFactory";  //改用UDP
  
  //std::string tcpTypeId = "ns3::TcpLinuxReno"; //这个是拥塞控制算法对应的组件名称，它装载在TcpL4Protocol::SocketType上，

  std::string topologyFile = "scratch/topo.txt";
  //需要详细观察代码看它是怎么被调用的，并新建一个符合我们要求的tcpType，它要根据routing table上的管子有没有堵住来决定发还是不发，
  //写完后我们要把这个拥塞控制算法换成修改后的tcpType


  //根据TCD的构想，网络内部的情况，绑在包上，ns3的packet类有个Tag属性，可以通过打Tag来把拥塞标记弄到包上，在网络边缘检测tag标记修改拥塞控制
  //bool isSack = true;
  //uint32_t delAckCount = 1;
  //std::string recovery = "ns3::TcpClassicRecovery"; 

  CommandLine cmd;
  //cmd.AddValue ("tcpTypeId", "TCP variant to use (e.g., ns3::TcpNewReno, ns3::TcpLinuxReno, etc.)", tcpTypeId);
  cmd.AddValue ("qdiscTypeId", "Queue disc for gateway (e.g., ns3::CoDelQueueDisc)", qdiscTypeId);
  cmd.AddValue ("topologyFile", "Path to the file describing the topology structure you need (e.g., scratch/topo.txt)", topologyFile);
  //cmd.AddValue ("segmentSize", "TCP segment size (bytes)", segmentSize);
  //cmd.AddValue ("delAckCount", "Delayed ack count", delAckCount);
  //cmd.AddValue ("enableSack", "Flag to enable/disable sack in TCP", isSack);
  cmd.AddValue ("stopTime", "Stop time for applications / simulation time will be stopTime", stopTime);
  //cmd.AddValue ("recovery", "Recovery algorithm type to use (e.g., ns3::TcpPrrRecovery", recovery);
  cmd.Parse (argc, argv);

  TypeId qdTid;
  NS_ABORT_MSG_UNLESS (TypeId::LookupByNameFailSafe (qdiscTypeId, &qdTid), "TypeId " << qdiscTypeId << " not found");

  /*
  // Set recovery algorithm and TCP variant
  Config::SetDefault ("ns3::TcpL4Protocol::RecoveryType", TypeIdValue (TypeId::LookupByName (recovery)));
  if (tcpTypeId.compare ("ns3::TcpWestwoodPlus") == 0)
    {
      // TcpWestwoodPlus is not an actual TypeId name; we need TcpWestwood here
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType", TypeIdValue (TcpWestwood::GetTypeId ()));
      // the default protocol type in ns3::TcpWestwood is WESTWOOD
      Config::SetDefault ("ns3::TcpWestwood::ProtocolType", EnumValue (TcpWestwood::WESTWOODPLUS));
    }
  else
    {
      TypeId tcpTid;
      NS_ABORT_MSG_UNLESS (TypeId::LookupByNameFailSafe (tcpTypeId, &tcpTid), "TypeId " << tcpTypeId << " not found");
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType", TypeIdValue (TypeId::LookupByName (tcpTypeId)));
    }*/


  // Create directories to store dat files
  struct stat buffer;
  int retVal;
  if ((stat (dir.c_str (), &buffer)) == 0)
    {
      std::string dirToRemove = "rm -rf " + dir;
      retVal = system (dirToRemove.c_str ());
      NS_ASSERT_MSG (retVal == 0, "Error in return value");
    }
  std::string dirToSave = "mkdir -p " + dir;
  retVal = system (dirToSave.c_str ());
  NS_ASSERT_MSG (retVal == 0, "Error in return value");
  retVal = system ((dirToSave + "/pcap/").c_str ());
  NS_ASSERT_MSG (retVal == 0, "Error in return value");
  retVal = system ((dirToSave + "/queueTraces/").c_str ());
  NS_ASSERT_MSG (retVal == 0, "Error in return value");
 // retVal = system ((dirToSave + "/cwndTraces/").c_str ());
 // NS_ASSERT_MSG (retVal == 0, "Error in return value");
  NS_UNUSED (retVal);

  std::cout << "cout test\n";
  build(topologyFile);

  std::cout << "cout test\n";

  // Calls function to check queue size
  Simulator::ScheduleNow (&CheckQueueSize, qd.Get (0));

  AsciiTraceHelper asciiTraceHelper;
  Ptr<OutputStreamWrapper> streamWrapper;

  // Create dat to store packets dropped and marked at the router
  streamWrapper = asciiTraceHelper.CreateFileStream (dir + "/queueTraces/drop-0.dat");
  qd.Get (0)->TraceConnectWithoutContext ("Drop", MakeBoundCallback (&DropAtQueue, streamWrapper));

  // Install packet sink at receiver side
  uint16_t port1 = 50000;
  //uint16_t port2 = 3;
  InstallUdpSever(nodes.Get (3), port1);
  //InstallPacketSink (nodes.Get (3), port1, socketFactory);
  //InstallPacketSink (nodes.Get (3), port2, socketFactory);

  // Install BulkSend application
  //InstallBulkSend (leftNodes.Get (0), routerToRightIPAddress [0].GetAddress (1), port, socketFactory, 2, 0, MakeCallback (&CwndChange));
  //InstallBulkSend (leftNodes.Get (0), routerToRightIPAddress [0].GetAddress (1), port, socketFactory);
  InstallOnOffSend (nodes.Get (0), IPAddresses [2].GetAddress (1), port1, socketFactory, 
                    "ns3::ConstantRandomVariable[Constant=1]", "ns3::ConstantRandomVariable[Constant=0]", 
                    1024, "1Mbps");

  globalOnoffTable->setGlobalNodes(nodes);


  // Enable PCAP on all the point to point interfaces
  channelHelpers[0].EnablePcapAll (dir + "pcap/ns-3", true);
  channelHelpers[m -1].EnablePcapAll (dir + "pcap/ns-3", true);
  
  Simulator::Stop (stopTime);
  Simulator::Run ();

  // Store queue stats in a file
  std::ofstream myfile;
  myfile.open (dir + "queueStats.txt", std::fstream::in | std::fstream::out | std::fstream::app);
  int queue_num = 0;
  for (int i = 0; i < n; ++i) {
    Ptr<ns3::Node> node = nodes.Get(i);
    uint32_t num = node -> GetNDevices();
    for (uint32_t k = 0; k + 1 != num; ++k) {
      Ptr<QueueDisc> queue= qd.Get(queue_num);
      myfile << "Stat for Queue " << i << "-" << k << ":\n";
      myfile << "Device MAC address: " << node->GetDevice(k)->GetAddress();
      myfile << qd.Get (queue_num)->GetStats ();
      queue_num ++;
    }  // Check queue size every 1/100 of a second
  myfile << std::endl;
  }
  myfile.close ();

  // Store configuration of the simulation in a file
  myfile.open (dir + "config.txt", std::fstream::in | std::fstream::out | std::fstream::app);
  myfile << "qdiscTypeId " << qdiscTypeId << "\n";
  myfile << "stream  " << stream << "\n";
  //myfile << "segmentSize " << segmentSize << "\n";
  //myfile << "delAckCount " << delAckCount << "\n";
  //myfile << "stopTime " << stopTime.As (Time::S) << "\n";
  myfile.close ();

  Simulator::Destroy ();

  return 0;
}
