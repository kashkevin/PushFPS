// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): the parts of keyboard and mouse capture that do not need
// Windows, so that they can be unit-tested: when the capture is on and when everything held has
// to be released, which devices typed, walking a Raw Input buffer and adding up its mouse
// reports. include/veyra/ui/RemotePlayKeyboardMouse.h is the Win32 side and stays thin.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace veyra::remoteplay {

// --- Capture state -------------------------------------------------------------------------------
// A key whose release never arrives stays pressed on the console. Every way out of a capture
// therefore releases everything, and so does the removal of a device that was in use. The Win32
// side reports events and carries out the actions; it holds no state of its own.
//
//   Released  --PollFront-->  Captured     (engage, start from nothing held)
//   Captured  --FocusLost / PollAway-->  Released    (disengage, release)
//   Captured  --ReleaseHotkey-->  Suspended          (disengage, release; Ctrl+Alt+Shift+Z)
//   Captured  --DeviceRemoved-->  Captured           (release)
//   Suspended --FocusLost / PollAway-->  Released    (the next PollFront captures again)
//   any       --SessionEnded / WindowChanged-->  Released   (disengage and release when captured; restart)
class KbmCaptureState {
public:
    enum class Event : std::uint8_t {
        PollFront,      // a pad poll with the stream window focused and in the foreground
        PollAway,       // a pad poll without that
        FocusLost,      // WM_KILLFOCUS, WM_ACTIVATE(inactive), the application left the foreground
        ReleaseHotkey,  // Ctrl+Alt+Shift+Z
        SessionEnded,   // the stream stopped or became view-only
        WindowChanged,  // the stream moved to another window
        DeviceRemoved,  // a keyboard or mouse that sent input during this capture was unplugged
        EngageFailed    // the engage action could not be carried out
    };
    enum class State : std::uint8_t {Released,Captured,Suspended};
    struct Actions {
        bool engage=false;        // register Raw Input, hide and clip the cursor
        bool disengage=false;     // undo that
        bool releaseInputs=false; // release every key and button, centre the sticks, drop pending movement
        bool restart=false;       // also go back to the first profile: the next session starts on foot
        bool operator==(const Actions&)const=default;
    };
    State state()const noexcept{return state_;}
    bool captured()const noexcept{return state_==State::Captured;}
    Actions on(Event event) noexcept{
        Actions out;
        const bool ends=event==Event::SessionEnded||event==Event::WindowChanged;
        switch(state_){
        case State::Released:
            if(event==Event::PollFront){state_=State::Captured;out.engage=out.releaseInputs=true;}
            break;
        case State::Captured:
            if(event==Event::FocusLost||event==Event::PollAway||ends){state_=State::Released;out.disengage=out.releaseInputs=true;}
            else if(event==Event::ReleaseHotkey){state_=State::Suspended;out.disengage=out.releaseInputs=true;}
            else if(event==Event::DeviceRemoved)out.releaseInputs=true;
            else if(event==Event::EngageFailed){state_=State::Released;out.releaseInputs=true;}
            break;
        case State::Suspended:
            if(event==Event::FocusLost||event==Event::PollAway||ends)state_=State::Released;
            break;
        }
        out.restart=ends;
        return out;
    }
private:
    State state_=State::Released;
};

// The keyboards and mice that sent input during a capture, by their Raw Input handle. Removing
// one of them releases everything; removing a device that was never used (a headset's media
// keys, a second mouse) does not interrupt the game. 0 is "no device" (injected input) and is
// never kept. When more devices typed than fit, it can no longer tell and says yes to all.
class KbmDeviceSet {
public:
    static constexpr std::size_t kCapacity=8;
    void note(std::uintptr_t id) noexcept{
        if(!id||overflowed_||find(id)<count_)return;
        if(count_==kCapacity){overflowed_=true;return;}
        ids_[count_++]=id;
    }
    bool contains(std::uintptr_t id)const noexcept{return overflowed_||(id&&find(id)<count_);}
    // True when the device had been in use.
    bool remove(std::uintptr_t id) noexcept{
        if(overflowed_)return true;
        const std::size_t at=id?find(id):count_;
        if(at>=count_)return false;
        ids_[at]=ids_[--count_];
        return true;
    }
    void clear() noexcept{count_=0;overflowed_=false;}
    std::size_t size()const noexcept{return count_;}
private:
    std::size_t find(std::uintptr_t id)const noexcept{
        std::size_t i=0;
        while(i<count_&&ids_[i]!=id)++i;
        return i;
    }
    std::array<std::uintptr_t,kCapacity> ids_{};
    std::size_t count_=0;
    bool overflowed_=false;
};

// --- Raw Input: buffered read ---------------------------------------------------------------
// GetRawInputBuffer fills a buffer with RAWINPUT blocks of different sizes. Each block starts
// on a pointer-size boundary; NEXTRAWINPUTBLOCK steps over one with the arithmetic below. The
// macro trusts dwSize, so the caller checks every block with rawInputBlockFits() first: a block
// that is too small for its payload or leaves the buffer ends the walk.
constexpr std::size_t rawInputNextOffset(std::size_t offset,std::size_t blockSize,std::size_t align=sizeof(void*)) noexcept{
    return (offset+blockSize+align-1)/align*align;
}
constexpr bool rawInputBlockFits(std::size_t offset,std::size_t blockSize,std::size_t minimumSize,std::size_t capacity) noexcept{
    return blockSize>=minimumSize&&offset<=capacity&&blockSize<=capacity-offset;
}

// The mouse reports of one WM_INPUT message and of everything drained after it, as one movement.
// Absolute-mode reports (tablets, remote desktop, some virtual machines) give a position, not a
// distance: they are counted and left out.
struct RawMouseBatch {
    static constexpr unsigned kAbsolute=0x01,kVirtualDesktop=0x02; // MOUSE_MOVE_ABSOLUTE, MOUSE_VIRTUAL_DESKTOP
    std::int64_t dx=0,dy=0;
    unsigned reports=0,absolute=0,moved=0;   // moved: relative reports with movement (KbmMouseRate)
    void add(unsigned flags,std::int32_t x,std::int32_t y) noexcept{
        ++reports;
        if(flags&kAbsolute){++absolute;return;}
        if(x!=0||y!=0)++moved;
        dx+=x;dy+=y;
    }
    bool empty()const noexcept{return dx==0&&dy==0;}
    int x()const noexcept{return clamped(dx);}
    int y()const noexcept{return clamped(dy);}
private:
    static int clamped(std::int64_t v) noexcept{
        const std::int64_t limit=std::numeric_limits<int>::max();
        return static_cast<int>(std::clamp(v,-limit,limit));
    }
};
// What the capture saw, for one log line when it ends: with a 1000 Hz mouse `largest` shows how
// many reports had queued up behind a single message.
struct RawInputStats {
    std::uint64_t messages=0,reports=0,absolute=0,drains=0;
    unsigned largest=0;
    void message(const RawMouseBatch& batch,bool drained) noexcept{
        ++messages;reports+=batch.reports;absolute+=batch.absolute;if(drained)++drains;
        largest=std::max(largest,batch.reports);
    }
};
} // namespace veyra::remoteplay
