#pragma once

// PS2X(mpeg-cost): opt-in timing for the host side of movie playback.
//
// All timers are inert unless PS2X_MPEG_COST is set to a value other than
// "0".  With the switch off, constructing a Step only reads a cached bool and
// none of the clock, allocation or logging code runs.  The slow-step line is
// deliberately compatible with tools/mpeg_cost.py:
//
//   [mpeg-cost] step=decode ms=41.237 pts=450450 frame=123 vsync=4567 t=34.567 at=35012
//
// `t` is host-monotonic seconds since the cost clock's anchor (process start);
// `at` is the same instant in milliseconds.  The summary line aggregates every
// completed step once per second:
//
//   [mpeg-cost] summary t=35.012 at=35450 steps=61 step=decode n=60 totalMs=... maxMs=...

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace ps2x::mpeg_cost
{
    struct Context
    {
        int64_t pts90k = -1;
        uint32_t frame = 0u;
        uint64_t vsync = 0u;
    };

    inline bool enabled() noexcept
    {
        static const bool value = []()
        {
            const char *env = std::getenv("PS2X_MPEG_COST");
            return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
        }();
        return value;
    }

    inline uint64_t nowNs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // PS2X(mpeg-cost): one anchor for the whole process, close enough to the
    // run's start for the audio underrun's SDL milliseconds to line up.  With
    // the switch off the initializer only reads the cached flag, so startup
    // does not pay for a clock read it will never use.
    inline const uint64_t kAnchorNs = enabled() ? nowNs() : 0u;

    namespace detail
    {
        struct Stat
        {
            uint64_t count = 0u;
            uint64_t totalNs = 0u;
            uint64_t maxNs = 0u;
        };

        struct State
        {
            Context context;
            std::unordered_map<std::string, Stat> stats;
            uint64_t nextSummaryNs = 0u;
        };

        inline State &state() noexcept
        {
            static State instance;
            return instance;
        }

        inline std::mutex &stateMutex() noexcept
        {
            static std::mutex instance;
            return instance;
        }

        inline double secondsSinceAnchor(uint64_t ns) noexcept
        {
            return static_cast<double>(ns - kAnchorNs) / 1000000000.0;
        }

        inline void writeSummaryLocked(uint64_t now, State &state)
        {
            uint64_t steps = 0u;
            for (const auto &[name, stat] : state.stats)
            {
                (void)name;
                steps += stat.count;
            }
            if (steps == 0u)
            {
                return;
            }

            std::ostringstream out;
            out << "[mpeg-cost] summary t=" << std::fixed << std::setprecision(3)
                << secondsSinceAnchor(now)
                << " at=" << (now - kAnchorNs) / 1000000ull
                << " steps=" << steps;
            for (const auto &[name, stat] : state.stats)
            {
                out << " step=" << name
                    << " n=" << stat.count
                    << " totalMs=" << (static_cast<double>(stat.totalNs) / 1000000.0)
                    << " maxMs=" << (static_cast<double>(stat.maxNs) / 1000000.0);
            }
            std::cerr << out.str() << std::endl;
            state.stats.clear();
        }
    }

    inline void note(const char *step, uint64_t startNs, const Context &context) noexcept
    {
        if (!enabled() || step == nullptr || startNs == 0u)
        {
            return;
        }

        try
        {
            const uint64_t now = nowNs();
            const uint64_t durationNs = now > startNs ? now - startNs : 0u;
            std::lock_guard<std::mutex> lock(detail::stateMutex());
            detail::State &state = detail::state();

            detail::Stat &stat = state.stats[step];
            ++stat.count;
            stat.totalNs += durationNs;
            if (durationNs > stat.maxNs)
            {
                stat.maxNs = durationNs;
            }

            if (state.nextSummaryNs == 0u)
            {
                state.nextSummaryNs = now + 1000000000ull;
            }

            if (durationNs > 8000000ull)
            {
                std::ostringstream out;
                out << "[mpeg-cost] step=" << step
                    << " ms=" << std::fixed << std::setprecision(3)
                    << (static_cast<double>(durationNs) / 1000000.0)
                    << " pts=" << context.pts90k
                    << " frame=" << context.frame
                    << " vsync=" << context.vsync
                    << " t=" << detail::secondsSinceAnchor(now)
                    << " at=" << (now - kAnchorNs) / 1000000ull;
                std::cerr << out.str() << std::endl;
            }

            if (now >= state.nextSummaryNs)
            {
                detail::writeSummaryLocked(now, state);
                state.nextSummaryNs = now + 1000000000ull;
            }
        }
        catch (...)
        {
            // Diagnostics must never take the game down.
        }
    }

    inline void note(const char *step, uint64_t startNs) noexcept
    {
        if (!enabled() || step == nullptr || startNs == 0u)
        {
            return;
        }
        Context context;
        try
        {
            std::lock_guard<std::mutex> lock(detail::stateMutex());
            context = detail::state().context;
        }
        catch (...)
        {
            return;
        }
        note(step, startNs, context);
    }

    inline void setContext(const Context &context) noexcept
    {
        if (!enabled())
        {
            return;
        }
        try
        {
            std::lock_guard<std::mutex> lock(detail::stateMutex());
            detail::state().context = context;
        }
        catch (...)
        {
        }
    }

    inline Context context() noexcept
    {
        Context result;
        if (!enabled())
        {
            return result;
        }
        try
        {
            std::lock_guard<std::mutex> lock(detail::stateMutex());
            result = detail::state().context;
        }
        catch (...)
        {
        }
        return result;
    }

    class Step
    {
    public:
        explicit Step(const char *name) noexcept
            : m_name(name), m_active(false)
        {
            if (!enabled() || name == nullptr)
            {
                return;
            }
            m_active = true;
            m_startNs = nowNs();
            m_context = context();
        }

        Step(const Step &) = delete;
        Step &operator=(const Step &) = delete;

        ~Step()
        {
            finish();
        }

        void finish() noexcept
        {
            if (!m_active)
            {
                return;
            }
            m_active = false;
            note(m_name, m_startNs, m_context);
        }

        bool active() const noexcept
        {
            return m_active;
        }

    private:
        const char *m_name;
        uint64_t m_startNs = 0u;
        Context m_context;
        bool m_active;
    };
}
