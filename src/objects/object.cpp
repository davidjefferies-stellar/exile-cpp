#include "objects/object.h"
#include "objects/object_data.h"

uint8_t Object::weight() const {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) return 3;
    return object_types_flags[idx] & ObjectTypeFlags::WEIGHT_MASK;
}
