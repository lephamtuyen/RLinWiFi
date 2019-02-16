#include "ns3/core-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/opengym-module.h"
//#include "ns3/csma-module.h"
#include "ns3/propagation-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/yans-wifi-channel.h"

#include <fstream>
#include <string>
#include <math.h>
#include <ctime> //timestampi
#include <iomanip> // put_time

using namespace std;
using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("OpenGym");

void installTrafficGenerator(Ptr<ns3::Node> fromNode, Ptr<ns3::Node> toNode, int port, string offeredLoad);
void PopulateARPcache ();

double envStepTime = 1;
double simulationTime = 10; //seconds
int CW = 0;

/*
Define observation space
*/
Ptr<OpenGymSpace> MyGetObservationSpace(void)
{
  uint32_t nodeNum = NodeList::GetNNodes ();
  float low = 0.0;
  float high = 10.0;
  std::vector<uint32_t> shape = {nodeNum,};
  std::string dtype = TypeNameGet<uint32_t> ();
  Ptr<OpenGymBoxSpace> space = CreateObject<OpenGymBoxSpace> (low, high, shape, dtype);
  NS_LOG_UNCOND ("MyGetObservationSpace: " << space);
  return space;
}

/*
Define action space
*/
Ptr<OpenGymSpace> MyGetActionSpace(void)
{
  float low = 0.0;
  float high = 10.0;
  std::vector<uint32_t> shape = {1,};
  std::string dtype = TypeNameGet<float> ();
  Ptr<OpenGymBoxSpace> space = CreateObject<OpenGymBoxSpace> (low, high, shape, dtype);
  NS_LOG_UNCOND ("MyGetActionSpace: " << space);
  return space;
}

/*
Define game over condition
*/
bool MyGetGameOver(void)
{
  bool isGameOver = false;
  NS_LOG_UNCOND ("MyGetGameOver: " << isGameOver);
  return isGameOver;
}

/*
Define extra info. Optional
*/
std::string MyGetExtraInfo(void)
{
  std::string myInfo = "linear-wireless | ";
  myInfo += "CW: " + std::to_string(CW);

  NS_LOG_UNCOND("MyGetExtraInfo: " << myInfo);
  return myInfo;
}

bool SetCw(Ptr<Node> node, uint32_t cwMinValue=0, uint32_t cwMaxValue=0)
{
  CW = cwMinValue;
  Ptr<NetDevice> dev = node->GetDevice (0);
  Ptr<WifiNetDevice> wifi_dev = DynamicCast<WifiNetDevice> (dev);
  Ptr<WifiMac> wifi_mac = wifi_dev->GetMac ();
  Ptr<RegularWifiMac> rmac = DynamicCast<RegularWifiMac> (wifi_mac);
  PointerValue ptr;
  rmac->GetAttribute ("Txop", ptr);
  Ptr<Txop> txop = ptr.Get<Txop> ();

  // if both set to the same value then we have uniform backoff?
  if (cwMinValue != 0) {
    NS_LOG_DEBUG ("Set CW min: " << cwMinValue);
    txop->SetMinCw(cwMinValue);
  }

  if (cwMaxValue != 0) {
    NS_LOG_DEBUG ("Set CW max: " << cwMaxValue);
    txop->SetMaxCw(cwMaxValue);
  }
  return true;
}

/*
Execute received actions
*/
bool MyExecuteActions(Ptr<OpenGymDataContainer> action)
{
  NS_LOG_UNCOND ("MyExecuteActions: " << action);

  Ptr<OpenGymBoxContainer<float> > box = DynamicCast<OpenGymBoxContainer<float> >(action);
  std::vector<float> actionVector = box->GetData();
  Config::Set("/$ns3::NodeListPriv/NodeList/*/$ns3::Node/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_Txop/$ns3::QosTxop/MinCw",UintegerValue(actionVector.at(0)));
  Config::Set("/$ns3::NodeListPriv/NodeList/*/$ns3::Node/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_Txop/$ns3::QosTxop/MaxCw",UintegerValue(actionVector.at(0)));
  return true;
}

void ScheduleNextStateRead(double envStepTime, Ptr<OpenGymInterface> openGymInterface)
{
  Simulator::Schedule (Seconds(envStepTime), &ScheduleNextStateRead, envStepTime, openGymInterface);
  openGymInterface->NotifyCurrentState();
}

/*
Define reward function
*/
uint64_t g_rxPktNum = 0;
void DestRxPkt (Ptr<const Packet> packet)
{
  NS_LOG_DEBUG ("Client received a packet of " << packet->GetSize () << " bytes");
  g_rxPktNum++;
}

float MyGetReward(void)
{
  static float lastValue = 0.0;
  float reward = g_rxPktNum - lastValue;
  lastValue = g_rxPktNum;
  return reward*(1500-20-8-8)* 8.0 / 1000 / 1000;
//   return reward;
}

/*
Collect observations
*/
Ptr<WifiMacQueue> GetQueue(Ptr<Node> node)
{
  Ptr<NetDevice> dev = node->GetDevice (0);
  Ptr<WifiNetDevice> wifi_dev = DynamicCast<WifiNetDevice> (dev);
  Ptr<WifiMac> wifi_mac = wifi_dev->GetMac ();
  Ptr<RegularWifiMac> rmac = DynamicCast<RegularWifiMac> (wifi_mac);
  PointerValue ptr;
  rmac->GetAttribute ("Txop", ptr);
  Ptr<Txop> txop = ptr.Get<Txop> ();
  Ptr<WifiMacQueue> queue = txop->GetWifiMacQueue ();
  return queue;
}

Ptr<OpenGymDataContainer> MyGetObservation(void)
{
  uint32_t nodeNum = NodeList::GetNNodes ();
  std::vector<uint32_t> shape = {nodeNum,};
  Ptr<OpenGymBoxContainer<uint32_t> > box = CreateObject<OpenGymBoxContainer<uint32_t> >(shape);

  for (NodeList::Iterator i = NodeList::Begin (); i != NodeList::End (); ++i) {
    Ptr<Node> node = *i;
    Ptr<WifiMacQueue> queue = GetQueue (node);
    uint32_t value = queue->GetNPackets();
    box->AddValue(value);
  }

  NS_LOG_UNCOND ("MyGetObservation: " << box);
  return box;
}

int
main (int argc, char *argv[])
{
  bool verbose = false;
  int nWifi = 5;
  bool tracing = false;
  bool useRts = false;
  int mcs = 11; 
  int channelWidth = 20;    
  int guardInterval = 800;
  string offeredLoad = "150";
  int port = 1025;
  string outputCsv = "cw.csv";
  int rng = 1;
  int warmup = 1;

  uint32_t openGymPort = 5555;
  uint32_t simSeed = 1;

  CommandLine cmd;
  cmd.AddValue ("CW", "Value of Contention Window", CW);
  cmd.AddValue ("nWifi", "Number of wifi 802.11ax STA devices", nWifi);
  cmd.AddValue ("verbose", "Tell echo applications to log if true", verbose);
  cmd.AddValue ("tracing", "Enable pcap tracing", tracing);
  cmd.AddValue ("rng", "Number of RngRun", rng);
  cmd.AddValue ("simTime", "Simulation time in seconds. Default: 10s", simulationTime);
  cmd.AddValue ("envStepTime", "Step time in seconds. Default: 0.1s", envStepTime);


  cmd.Parse (argc,argv);

  NS_LOG_UNCOND("Ns3Env parameters:");
  NS_LOG_UNCOND("--simulationTime: " << simulationTime);
  NS_LOG_UNCOND("--openGymPort: " << openGymPort);
  NS_LOG_UNCOND("--envStepTime: " << envStepTime);
  NS_LOG_UNCOND("--seed: " << simSeed);

  if (verbose)
    {
     LogComponentEnable ("UdpEchoClientApplication", LOG_LEVEL_INFO);
     LogComponentEnable ("UdpEchoServerApplication", LOG_LEVEL_INFO);
    }

  if (useRts)
    {
      Config::SetDefault ("ns3::WifiRemoteStationManager::RtsCtsThreshold", StringValue ("0"));
    }

  Ptr<MatrixPropagationLossModel> lossModel = CreateObject<MatrixPropagationLossModel> ();
  lossModel->SetDefaultLoss (50);

  NodeContainer wifiStaNode;
  wifiStaNode.Create (nWifi);
  NodeContainer wifiApNode;
  wifiApNode.Create (1);

  YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
  Ptr<YansWifiChannel> chan = channel.Create();
  chan->SetPropagationLossModel(lossModel);
  chan->SetPropagationDelayModel (CreateObject <ConstantSpeedPropagationDelayModel> ());

  YansWifiPhyHelper phy = YansWifiPhyHelper::Default ();
  if (tracing){
      phy.SetPcapDataLinkType (WifiPhyHelper::DLT_IEEE802_11_RADIO);
  }
  phy.SetChannel (chan);

  // Set guard interval
  phy.Set ("GuardInterval", TimeValue (NanoSeconds (guardInterval)));

  WifiMacHelper mac;
  WifiHelper wifi;
  
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ax_5GHZ);

  std::ostringstream oss;
  oss << "HeMcs" << mcs;
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager","DataMode", StringValue (oss.str ()),
                               "ControlMode", StringValue (oss.str ()));
  
  //802.11ac PHY
 /*
  phy.Set ("ShortGuardEnabled", BooleanValue (0));
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ac);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
  "DataMode", StringValue ("VhtMcs8"),
  "ControlMode", StringValue ("VhtMcs8"));
 */
 //802.11n PHY
  //phy.Set ("ShortGuardEnabled", BooleanValue (1));
  //wifi.SetStandard (WIFI_PHY_STANDARD_80211n_5GHZ);
  //wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
  //                              "DataMode", StringValue ("HtMcs7"),
  //                              "ControlMode", StringValue ("HtMcs7"));

  Ssid ssid = Ssid ("ns3-80211ax");

  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (ssid),
                "ActiveProbing", BooleanValue (false),
                "BE_MaxAmpduSize", UintegerValue (0));

  NetDeviceContainer staDevice;
  staDevice = wifi.Install (phy, mac, wifiStaNode);

  mac.SetType ("ns3::ApWifiMac",
               "EnableBeaconJitter", BooleanValue (false),
               "Ssid", SsidValue (ssid));

  NetDeviceContainer apDevice;
  apDevice = wifi.Install (phy, mac, wifiApNode);

  // Set channel width
  Config::Set ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/ChannelWidth", UintegerValue (channelWidth));

  // mobility.
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();

  positionAlloc->Add (Vector (0.0, 0.0, 0.0));
  positionAlloc->Add (Vector (1.0, 0.0, 0.0));
  mobility.SetPositionAllocator (positionAlloc);

  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");

  mobility.Install (wifiApNode);
  mobility.Install (wifiStaNode);
  /* Internet stack*/
  InternetStackHelper stack;
  stack.Install (wifiApNode);
  stack.Install (wifiStaNode);
  //Random

  RngSeedManager::SetSeed(1);
  RngSeedManager::SetRun(rng);

  Ipv4AddressHelper address;
  address.SetBase ("192.168.1.0", "255.255.255.0");
  Ipv4InterfaceContainer staNodeInterface;
  Ipv4InterfaceContainer apNodeInterface;

  staNodeInterface = address.Assign (staDevice);
  apNodeInterface = address.Assign (apDevice);
 
  if (CW) {
    Config::Set("/$ns3::NodeListPriv/NodeList/*/$ns3::Node/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_Txop/$ns3::QosTxop/MinCw",UintegerValue(CW));
    Config::Set("/$ns3::NodeListPriv/NodeList/*/$ns3::Node/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_Txop/$ns3::QosTxop/MaxCw",UintegerValue(CW));
  }

  for(int i = 0; i < nWifi; ++i) {
    installTrafficGenerator(wifiStaNode.Get(i),wifiApNode.Get(0), port++, offeredLoad);
  }

  PopulateARPcache();
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  Ptr<FlowMonitor> monitor;
  FlowMonitorHelper flowmon;
  monitor = flowmon.InstallAll();
  monitor->SetAttribute ("StartTime", TimeValue (Seconds (warmup)));

  Ptr<OpenGymInterface> openGymInterface = CreateObject<OpenGymInterface> (openGymPort);
  openGymInterface->SetGetActionSpaceCb( MakeCallback (&MyGetActionSpace) );
  openGymInterface->SetGetObservationSpaceCb( MakeCallback (&MyGetObservationSpace) );
  openGymInterface->SetGetGameOverCb( MakeCallback (&MyGetGameOver) );
  openGymInterface->SetGetObservationCb( MakeCallback (&MyGetObservation) );
  openGymInterface->SetGetRewardCb( MakeCallback (&MyGetReward) );
  openGymInterface->SetGetExtraInfoCb( MakeCallback (&MyGetExtraInfo) );
  openGymInterface->SetExecuteActionsCb( MakeCallback (&MyExecuteActions) );

  Simulator::Schedule (Seconds(0.0), &ScheduleNextStateRead, envStepTime, openGymInterface);

  Simulator::Stop (Seconds (simulationTime + 1));

  Simulator::Run ();

  double flowThr;

  ofstream myfile;
  myfile.open (outputCsv, ios::app);

  /* Contents of CSV output file
  Timestamp, CW, nWifi, RngRun, SourceIP, DestinationIP, Throughput
  */
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();
  for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin (); i != stats.end (); ++i) {
      auto time = std::time(nullptr); //Get timestamp
      auto tm = *std::localtime(&time);
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (i->first);
      //flowThr=i->second.rxBytes * 8.0 / (i->second.timeLastRxPacket.GetSeconds () - i->second.timeFirstTxPacket.GetSeconds ()) / 1024 / 1024;
      flowThr = i->second.rxBytes * 8.0 / simulationTime / 1000 / 1000;
      NS_LOG_UNCOND ("Flow " << i->first  << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")\tThroughput: " <<  flowThr  << " Mbps\tTime: " << i->second.timeLastRxPacket.GetSeconds () - i->second.timeFirstTxPacket.GetSeconds () << " s\tRx packets " << i->second.rxPackets);
                myfile << std::put_time(&tm, "%Y-%m-%d %H:%M") << "," << CW << "," << nWifi << "," << RngSeedManager::GetRun() << "," << t.sourceAddress << "," << t.destinationAddress << "," << flowThr;
                myfile << std::endl;
        }
        myfile.close();
        
  Simulator::Destroy ();
  NS_LOG_UNCOND ("Packets registered by handler: " <<  g_rxPktNum  << " Packets" << endl);

  return 0;
}

// Traffic generator declaration from lte-wifi.cc
void installTrafficGenerator(Ptr<ns3::Node> fromNode, Ptr<ns3::Node> toNode, int port, string offeredLoad) {

        Ptr<Ipv4> ipv4 = toNode->GetObject<Ipv4> (); // Get Ipv4 instance of the node
        Ipv4Address addr = ipv4->GetAddress (1, 0).GetLocal (); // Get Ipv4InterfaceAddress of xth interface.

        ApplicationContainer sourceApplications, sinkApplications;

        uint8_t tosValue = 0x70; //AC_BE
        double simulationTime = 20;
        //Add random fuzz to app start time
        double min = 0.0;
        double max = 1.0;
        Ptr<UniformRandomVariable> fuzz = CreateObject<UniformRandomVariable> ();
        fuzz->SetAttribute ("Min", DoubleValue (min));
        fuzz->SetAttribute ("Max", DoubleValue (max));

        InetSocketAddress sinkSocket (addr, port);
        sinkSocket.SetTos (tosValue);
        //OnOffHelper onOffHelper ("ns3::TcpSocketFactory", sinkSocket);
        OnOffHelper onOffHelper ("ns3::UdpSocketFactory", sinkSocket);
        onOffHelper.SetConstantRate (DataRate (offeredLoad + "Mbps"), 1500-20-8-8);
        sourceApplications.Add (onOffHelper.Install (fromNode)); //fromNode

        //PacketSinkHelper packetSinkHelper ("ns3::TcpSocketFactory", sinkSocket);
        // PacketSinkHelper packetSinkHelper ("ns3::UdpSocketFactory", sinkSocket);
        UdpServerHelper sink (port);
        sinkApplications = sink.Install (toNode);
        // sinkApplications.Add (packetSinkHelper.Install (toNode)); //toNode

        sinkApplications.Start (Seconds (0.0));
        sinkApplications.Stop (Seconds (simulationTime + 1));

        Ptr<UdpServer> udpServer = DynamicCast<UdpServer>(sinkApplications.Get(0));
        udpServer->TraceConnectWithoutContext ("Rx", MakeCallback (&DestRxPkt));

        sourceApplications.Start (Seconds (1.0+fuzz->GetValue ()));
        sourceApplications.Stop (Seconds (simulationTime + 1));
}

void PopulateARPcache () {
	Ptr<ArpCache> arp = CreateObject<ArpCache> ();
	arp->SetAliveTimeout (Seconds (3600 * 24 * 365) );

	for (NodeList::Iterator i = NodeList::Begin (); i != NodeList::End (); ++i)
	{
		Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol> ();
		NS_ASSERT (ip !=0);
		ObjectVectorValue interfaces;
		ip->GetAttribute ("InterfaceList", interfaces);

		for (ObjectVectorValue::Iterator j = interfaces.Begin (); j != interfaces.End (); j++)
		{
			Ptr<Ipv4Interface> ipIface = (*j).second->GetObject<Ipv4Interface> ();
			NS_ASSERT (ipIface != 0);
			Ptr<NetDevice> device = ipIface->GetDevice ();
			NS_ASSERT (device != 0);
			Mac48Address addr = Mac48Address::ConvertFrom (device->GetAddress () );

			for (uint32_t k = 0; k < ipIface->GetNAddresses (); k++)
			{
				Ipv4Address ipAddr = ipIface->GetAddress (k).GetLocal();
				if (ipAddr == Ipv4Address::GetLoopback ())
					continue;

				ArpCache::Entry *entry = arp->Add (ipAddr);
				Ipv4Header ipv4Hdr;
				ipv4Hdr.SetDestination (ipAddr);
				Ptr<Packet> p = Create<Packet> (100);
				entry->MarkWaitReply (ArpCache::Ipv4PayloadHeaderPair (p, ipv4Hdr));
				entry->MarkAlive (addr);
			}
		}
	}

	for (NodeList::Iterator i = NodeList::Begin (); i != NodeList::End (); ++i)
	{
		Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol> ();
		NS_ASSERT (ip !=0);
		ObjectVectorValue interfaces;
		ip->GetAttribute ("InterfaceList", interfaces);

		for (ObjectVectorValue::Iterator j = interfaces.Begin (); j != interfaces.End (); j ++)
		{
			Ptr<Ipv4Interface> ipIface = (*j).second->GetObject<Ipv4Interface> ();
			ipIface->SetAttribute ("ArpCache", PointerValue (arp) );
		}
	}
}


