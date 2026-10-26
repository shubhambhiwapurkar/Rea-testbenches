#include "Simulation/SummitSimHandles.h"

#include "Sim/Avalanche.h"
#include "Sim/Balance.h"
#include "Sim/Climber.h"
#include "Sim/Random.h"
#include "Sim/Weather.h"

void FSummitSimDeleter::operator()(summit::sim::Climber* Ptr) const { delete Ptr; }
void FSummitSimDeleter::operator()(summit::sim::WeatherSystem* Ptr) const { delete Ptr; }
void FSummitSimDeleter::operator()(summit::sim::SnowpackModel* Ptr) const { delete Ptr; }
void FSummitSimDeleter::operator()(summit::sim::BalanceModel* Ptr) const { delete Ptr; }
void FSummitSimDeleter::operator()(summit::sim::Pcg32* Ptr) const { delete Ptr; }
