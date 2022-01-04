#include "tcp-lossless-tcd.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {
    NS_LOG_COMPONENT_DEFINE ("TcpLosslessTCD");
    NS_OBJECT_ENSURE_REGISTERED (TcpLosslessTCD);

    TypeId
    TcpLosslessTCD::GetTypeId (void) {
        static TypeId tid = TypeId ("ns3::TcpLosslessTCD")
            .SetParent<TcpCongestionOps> ()
            .SetGroupName ("Internet")
            .AddConstructor<TcpLosslessTCD> ()
        ;
        return tid;
    }

    TcpLosslessTCD::TcpLosslessTCD(void) : TcpCongestionOps () {
        NS_LOG_FUNCTION (this);
    }
}