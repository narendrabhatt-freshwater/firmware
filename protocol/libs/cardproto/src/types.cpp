#include "cardproto/types.hpp"

namespace cardproto
{

  std::string TargetPrefix(Target t)
  {
    switch (t)
    {
    case Target::Channel:
      return "c:";
    case Target::Effect:
      return "e:";
    case Target::All:
      return "*:";
    }
    return "*:";
  }

} // namespace cardproto
