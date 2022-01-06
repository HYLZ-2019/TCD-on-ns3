#include <vector>
#include <map>
#include <ns3/address.h>
#include <ns3/queue-disc.h>
#include <semaphore.h>
namespace ns3 {
/**
 * @brief A table recording on/off information of all nodes in a network, used for implementing a lossless network.
 * A single LosslessOnoffTable is used for an entire lossless network.
 * "LosslessQueueDisc"s interact with a LosslessOnoffTable to update on/off statuses. 
 */
class LosslessOnoffTable {
public:
    void globalInit();

    void addNetDevice(Address addr); //把某个device addr放到list里

    void setValue(Address addr, bool value); // if OFF -> ON  RUN

    bool getValue(Address addr);

    void blockQueueAdding(Address addr, Ptr<QueueDisc> qdisc);

private:
    sem_t mutex;
    sem_t BQ;

    std::map <Address, bool> ONOFFlist;
    std::multimap <Address, Ptr<QueueDisc> > blockQueue;
};

} // namespace ns3