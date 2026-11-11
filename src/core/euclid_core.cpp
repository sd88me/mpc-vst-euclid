// The pattern table of euclid_core.h. Kept in its own file because bjlund.h says `using namespace std`.
#include "euclid_core.h"

#include <mutex>
#include <vector>

#include "bjlund.h"

namespace euclid {

const PatternTable& PatternTable::get()
{
    static PatternTable* t = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        PatternTable* x = new PatternTable();   // never freed: no static destructor at unload
        std::memset(x->bits, 0, sizeof x->bits);
        for (int steps = 2; steps <= 64; steps++)
            for (int fill = 1; fill <= 64; fill++)
            {
                const std::vector<int> v = BJLUND::bjlund(fill, steps);   // not re-entrant (globals): only ever run here
                uint64_t b = 0;
                for (size_t i = 0; i < v.size() && i < 64; i++) if (v[i]) b |= 1ull << i;
                x->bits[steps][fill] = b;
            }
        t = x;
    });
    return *t;
}

} // namespace euclid
