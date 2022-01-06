#include <vector>
#include <ns3/address.h>
#include <ns3/queue-disc.h>
#include <semaphore.h>
using namespace std;

sem_t mutex;
sem_t BQ;

map <Address, bool> ONOFFlist;
multimap <Address, Ptr<QueueDisc> > blockQueue;

void globalInit();

void addNetDevice(Address addr); //把某个device addr放到list里

void setValue(Address addr, bool value); // if OFF -> ON  RUN

bool getValue(Address addr);

void blockQueueAdding(Address addr, Ptr<QueueDisc> qdisc);