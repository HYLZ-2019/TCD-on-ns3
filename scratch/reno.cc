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
Time stopTime = Seconds (20);
Time ColdStartBegin = Seconds (1);
Time ColdStartEnd = Seconds (10);
//Time EventstopTime = Seconds (200);
uint32_t segmentSize = 524;


// Function to calculate drops in a particular Queue
// Shouldn't be called in our case, delete it later
static void
DropAtQueue (Ptr<OutputStreamWrapper> stream, Ptr<const QueueDiscItem> item)
{
  *stream->GetStream () << Simulator::Now ().GetSeconds () << " 1" << std::endl;
}

void InstallUdpServer(Ptr<Node> node, uint16_t port, Time stTime, Time ndTime) 
{
  UdpServerHelper server (port);
  ApplicationContainer apps = server.Install (node);
  apps.Start (stTime);
  apps.Stop (ndTime);
}

void InstallUdpClient(Ptr<Node> node, Address addr, uint16_t port, Time stTime, Time ndTime, Time interval, uint32_t MaxPacketSize, uint32_t maxPacketCount)
{
  std::cout << node << " " << addr << " " << port << " " << stTime.GetMilliSeconds() << " "<< ndTime.GetMilliSeconds() << " "
            << interval.GetMicroSeconds() << " " <<MaxPacketSize << " " << maxPacketCount << "\n";
  UdpClientHelper client (addr, port);
  client.SetAttribute ("MaxPackets", UintegerValue (maxPacketCount));
  client.SetAttribute ("Interval", TimeValue (interval));
  client.SetAttribute ("PacketSize", UintegerValue (MaxPacketSize));
  ApplicationContainer apps = client.Install (node);
  apps.Start (stTime);
  apps.Stop (ndTime);
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

std::string qdiscTypeId = "ns3::LosslessQueueDisc"; 


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
      QueueDisc* qptr = &(*queue);
      LosslessQueueDisc* lqueue = (LosslessQueueDisc*) qptr;
      uint32_t qSize = queue->GetCurrentSize ().GetValue ();
      std::ofstream fPlotQueue (std::stringstream (dir + "queue-" + std::to_string(i) + "-" + std::to_string(k) + ".dat").str ().c_str (), std::ios::out | std::ios::app);
      fPlotQueue << Simulator::Now ().GetSeconds () << " " << qSize <<" ";
      bool on = queue->onoffTable->getValue(dev->GetAddress());
      if (on){
        fPlotQueue << "[ON] ";
      }
      else{
        fPlotQueue << "[OFF] ";
      }
      fPlotQueue << "Transmitted: " << lqueue->getPacketsTransmitted() << "\n";
      fPlotQueue.close ();
      queue_num ++;
    }  // Check queue size every 1/100 of a second
  }
  Simulator::Schedule (Seconds (1), &CheckQueueSize, queue);
}

/** Build up a network according to the configurations in *filename*.
 * 
 */
void buildNetwork(std::string filename) {
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


/**
 * @brief Install UDP clients and servers according to the input file.
 * File format:
 * [number of servers]
 * [server node number 1] [server port number 1]
 * [server node number 2] [server port number 2]
 * ....
 * [number of clients]
 * [client node number 1] [channel number the server is on] [channel end the server is on] [server port number 1] [double interval (in seconds)] [uint32_t MaxPacketSize] [uint32_t maxPacketCount] [double start time of this flow] [double end time of this flow] [int isperiodic] [double period]
 * ....
 *  
 * About [channel & server]:
 * Suppose the input in topo.txt is:
 * 3 2
 * 0 1 1Mbps 50ms 10.0.0.0 255.255.255.0
 * 1 2 8Kbps 200ms 10.1.0.0 255.255.255.0
 * 
 * Then a server built on node 1 can be refered to as 
 * "server channel number = 0 (first channel line) & server channel end = 1 (on right side of channel)"
 * or
 * "server channel number = 1 (second channel line) & server channel end = 0 (on left side of channel)"
 * ....
 * @param filename 
 */
void ColdStart() {
  int port_num = 0;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < m; ++j) {
      for (int k = 0; k < 2; ++k) {
        ++port_num;
        InstallUdpClient(nodes.Get (i), IPAddresses [j].GetAddress (k), port_num, ColdStartBegin, ColdStartBegin + Seconds(0.5), Seconds(1), 64, 64);
      }
    }
  }
}

void installApps(std::string filename) {
  std::freopen(filename.c_str(), "r", stdin);
  int serverNum, clientNum;
  
  std::cin >> serverNum;  
  for (int i=0; i<serverNum; i++){
    int nodenum, portnum;
    std::cin >> nodenum >> portnum;
    InstallUdpServer(nodes.Get(nodenum), portnum, ColdStartEnd, stopTime);
  }

  std::cin >> clientNum;
  for (int i=0; i<clientNum; i++){
    int clinode, servChannelSeq, servChannelEnd, servport;
    double interval, stTime, ndTime, period;
    int maxsize, maxcnt, isperiodic;
    std::cin >> clinode >> servChannelSeq >> servChannelEnd >> servport >> interval >> maxsize >> maxcnt >> stTime >> ndTime >> isperiodic >> period;
    std::cout << interval << "!!!!!!!!!!!!!!!!!!!!!!\n";
    stTime += ColdStartEnd.GetMilliSeconds();
    ndTime += ColdStartEnd.GetMilliSeconds();
    InstallUdpClient(nodes.Get (clinode), IPAddresses [servChannelSeq].GetAddress (servChannelEnd), servport, MilliSeconds(stTime), MilliSeconds(ndTime), NanoSeconds(interval * 1000), maxsize, maxcnt);
    if (isperiodic) {
      stTime += period, ndTime += period;
      for (; MilliSeconds(stTime + period) < stopTime; stTime += period, ndTime += period) {
        InstallUdpClient(nodes.Get (clinode), IPAddresses [servChannelSeq].GetAddress (servChannelEnd), servport, MilliSeconds(stTime), MilliSeconds(ndTime), NanoSeconds(interval * 1000), maxsize, maxcnt);
      }
    }
  }

  return;
}

int main (int argc, char *argv[])
{
  globalOnoffTable = new LosslessOnoffTable();
  globalOnoffTable->globalInit();

  uint32_t stream = 1;
  std::string transportProt = "Udp";
  std::string socketFactory = "ns3::UdpSocketFactory";  //改用UDP

  std::string topologyFile = "scratch/topo.txt";
  std::string appsFile = "scratch/apps.txt";

  CommandLine cmd;
  cmd.AddValue ("qdiscTypeId", "Queue disc for gateway (e.g., ns3::CoDelQueueDisc)", qdiscTypeId);
  cmd.AddValue ("topologyFile", "Path to the file describing the network topology structure you need (e.g., scratch/topo.txt)", topologyFile);
  cmd.AddValue ("appsFile", "Path to the file describing how UDP server/client applications are installed.", appsFile);
  cmd.AddValue ("stopTime", "Stop time for applications / simulation time will be stopTime", stopTime);
  cmd.AddValue ("output", "Directory to hold output results", dir);
  cmd.Parse (argc, argv);

  TypeId qdTid;
  NS_ABORT_MSG_UNLESS (TypeId::LookupByNameFailSafe (qdiscTypeId, &qdTid), "TypeId " << qdiscTypeId << " not found");

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

  NS_UNUSED (retVal);

  buildNetwork(topologyFile);

  // Calls function to check queue size
  Simulator::ScheduleNow (&CheckQueueSize, qd.Get (0));

  AsciiTraceHelper asciiTraceHelper;
  Ptr<OutputStreamWrapper> streamWrapper;

  // Create dat to store packets dropped and marked at the router
  streamWrapper = asciiTraceHelper.CreateFileStream (dir + "/queueTraces/drop-0.dat");
  qd.Get (0)->TraceConnectWithoutContext ("Drop", MakeBoundCallback (&DropAtQueue, streamWrapper));
  
  ColdStart();
  installApps(appsFile);

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
      myfile << "Stat for Queue " << i << "-" << k << ":";
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
