/*
 * TCP model for paper https://dl.acm.org/doi/10.1145/3452296.3472899
 */

#ifndef TCPLOSSLESSTCD_H
#define TCPLOSSLESSTCD_H

#include "ns3/tcp-congestion-ops.h"
#include "ns3/tcp-socket-state.h"

namespace ns3 {


class TcpLosslessTCD: public TcpCongestionOps {
public:
    static TypeId GetTypeId(void);

    TcpLosslessTCD();

}

#endif // TCPLOSSLESSTCD_H