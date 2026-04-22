#ifndef GS_RESIDENT_INSTANCE_CONTRACT_PUBLISHER_H
#define GS_RESIDENT_INSTANCE_CONTRACT_PUBLISHER_H

#include "core/string/ustring.h"

class GaussianSplatRenderer;

namespace ResidentInstanceContractPublisher {

bool publish_resident_direct_data_contract(GaussianSplatRenderer *p_renderer, bool p_allow_empty_instance_bootstrap, String *r_reason = nullptr);

} // namespace ResidentInstanceContractPublisher

#endif // GS_RESIDENT_INSTANCE_CONTRACT_PUBLISHER_H
