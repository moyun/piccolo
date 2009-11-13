#ifndef KERNEL_H_
#define KERNEL_H_

#include "util/common.h"

namespace upc {

typedef void (*KernelFunction)(void);

struct KernelRegistry {
  struct StaticHelper {
    StaticHelper(const char* name, KernelFunction kf);
  };

  static KernelFunction get_kernel(int id);
  static int get_id(KernelFunction kf);

  static map<int, KernelFunction>* get_mapping();
};



#define REGISTER_KERNEL(kf)\
  namespace {\
    struct MyStaticHelper : public KernelRegistry::StaticHelper {\
      MyStaticHelper() : StaticHelper(#kf, kf) {}\
    };\
    static MyStaticHelper registryHelper;\
  }

} // end namespace
#endif /* KERNEL_H_ */
