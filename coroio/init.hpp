#pragma once

namespace NNet {

class TInitializer {
public:
    TInitializer();
#ifdef _WIN32
    ~TInitializer();
#endif
};

} // namespace NNet
