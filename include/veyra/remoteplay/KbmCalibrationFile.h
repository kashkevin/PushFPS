// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): where a calibration's profile goes and how it is written.
// Portable and unit-tested (std::filesystem); docs/KEYBOARD_MOUSE.md, "Calibrating a game".
//
//   VEYRA_EXP_RP_KBM_PROFILE set:  the calibration writes into that file.
//   unset:                          slipstream-kbm-<game>.txt next to settings.ini, where <game> is
//                                   the `game=` line of slipstream-kbm.ini in the same folder (the
//                                   player writes it; the game is never detected), or "default".
//                                   The same file is loaded at the next start when it exists.
//
// The file keeps every line it had. What the calibration measured goes into one block between two
// marker lines at the end; a new calibration replaces that block (later lines win when the file is
// read, so the block overrides the same settings above it). An existing file is first copied to
// <file>.bak; when that copy fails nothing is written. The new text goes to <file>.new and is then
// renamed over the file, so a crash leaves the old file or the new one, never half of one.
#include "KbmCalibration.h"
#include "KbmProfile.h"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
namespace veyra::remoteplay {

inline constexpr std::string_view kKbmCalibrationBegin="# --- calibration (written by Slipstream; edit above this line or calibrate again) ---";
inline constexpr std::string_view kKbmCalibrationEnd="# --- end of calibration ---";
inline constexpr std::string_view kKbmGameSettingsFile="slipstream-kbm.ini";

// The `game=` line of slipstream-kbm.ini (or of any profile text): the first one that is plain
// text of 1 to 64 characters. Empty when there is none.
inline std::string kbmGameName(std::string_view text){
    while(!text.empty()){
        const auto end=text.find('\n');
        std::string_view line=text.substr(0,end);
        text=end==std::string_view::npos?std::string_view{}:text.substr(end+1);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        line=kbmprofile::trim(line);
        if(line.size()>=3&&line.substr(0,3)=="\xEF\xBB\xBF")line.remove_prefix(3);
        const auto equals=line.find('=');
        if(equals==std::string_view::npos||kbmprofile::lowered(kbmprofile::trim(line.substr(0,equals)))!="game")continue;
        const auto value=kbmprofile::trim(line.substr(equals+1));
        bool plain=!value.empty()&&value.size()<=kKbmProfileMaxName;
        for(const char c:value){const auto b=static_cast<unsigned char>(c);if(b<0x20||b>=0x7F)plain=false;}
        if(plain)return std::string(value);
    }
    return {};
}
// "GTA 6" -> "gta-6": lower-case letters and digits, one '-' for every other run, at most 48.
inline std::string kbmGameSlug(std::string_view name){
    std::string out;
    for(const char c:name){
        const char lower=c>='A'&&c<='Z'?char(c-'A'+'a'):c;
        if((lower>='a'&&lower<='z')||(lower>='0'&&lower<='9'))out+=lower;
        else if(!out.empty()&&out.back()!='-')out+='-';
        if(out.size()>=48)break;
    }
    while(!out.empty()&&out.back()=='-')out.pop_back();
    return out;
}
inline std::string kbmCalibratedFileName(std::string_view game){
    const std::string slug=kbmGameSlug(game);
    return "slipstream-kbm-"+(slug.empty()?std::string("default"):slug)+".txt";
}

// The block for what was measured: only measured numbers, each inside the parser's range.
inline std::string kbmCalibrationBlock(const KbmProfileSet& calibrated,const std::array<KbmCalibrationMeasured,3>& measured){
    const auto n=[](double v,int decimals){return KbmCalibration::formatNumber(v,decimals);};
    std::string out(kKbmCalibrationBegin);out+="\n";
    out+="# The console sees an analog stick: these numbers undo the game's stick response as far as it was measured.\n";
    if(!calibrated.game.empty())out+="game="+calibrated.game+"\n";
    out+="counts_per_360="+n(kbmCountsPer360(calibrated),1)+"\n";
    for(std::size_t i=0;i<3;++i){
        const auto& m=measured[i];
        if(!m.any())continue;
        const auto& aim=calibrated.profiles[i].aim;
        const std::string p=std::string(kbmProfileName(KbmProfileId(i)))+".";
        if(m.deadzone)out+=p+"deadzone="+n(aim.deadzone,4)+"\n";
        if(m.turnSeconds){
            out+="# "+p.substr(0,p.size()-1)+": full turn "+n(*m.turnSeconds,3)+" s at full tilt\n";
            out+=p+"full="+n(aim.fullSpeedCounts,1)+"\n"+p+"sens_x="+n(aim.sensitivityX,4)+"\n"+p+"carry_ms="+n(aim.carryMs,1)+"\n";
        }
        if(m.turnSeconds||m.ratio)out+=p+"sens_y="+n(aim.sensitivityY,4)+"\n";
        if(m.response[0]||m.response[1]||m.response[2]){
            out+=p+"curve="+n(aim.curve,4)+"\n";
            if(MouseAim::table(aim))
                out+=p+"curve_25="+n(aim.curvePoints[0],4)+"\n"+p+"curve_50="+n(aim.curvePoints[1],4)+"\n"+p+"curve_75="+n(aim.curvePoints[2],4)+"\n";
            else out+=p+"curve_25=0\n"+p+"curve_50=0\n"+p+"curve_75=0\n";
        }
    }
    out+=kKbmCalibrationEnd;out+="\n";
    return out;
}
// `existing` without its old calibration block, with `block` at the end.
inline std::string kbmMergeCalibration(std::string_view existing,std::string_view block){
    std::string kept;
    bool inside=false;
    while(!existing.empty()){
        const auto end=existing.find('\n');
        const std::string_view raw=existing.substr(0,end+(end==std::string_view::npos?0:1));
        existing=end==std::string_view::npos?std::string_view{}:existing.substr(end+1);
        std::string_view line=raw;
        while(!line.empty()&&(line.back()=='\n'||line.back()=='\r'))line.remove_suffix(1);
        if(!inside&&line==kKbmCalibrationBegin){inside=true;continue;}
        if(inside){if(line==kKbmCalibrationEnd)inside=false;continue;}
        kept+=raw;
    }
    if(!kept.empty()&&kept.back()!='\n')kept+="\n";
    kept+=block;
    return kept;
}
// A new file's first lines.
inline std::string kbmNewProfileText(const KbmProfileSet& set){
    std::string out="# Slipstream keyboard and mouse profile (docs/KEYBOARD_MOUSE.md).\n";
    out+="name="+(set.game.empty()?std::string("calibrated"):set.game+" (calibrated)").substr(0,kKbmProfileMaxName)+"\n";
    return out;
}

struct KbmWriteResult {bool ok=false,backedUp=false,backupKept=false;std::string error;};
// Writes `text` to `path`, keeping the old file as <path>.bak. The FIRST backup is kept: an existing
// .bak (the player's file from before any calibration) is never overwritten (`backupKept`). A text
// over the profile size limit is refused: it could not be read back whole.
inline KbmWriteResult kbmWriteProfile(const std::filesystem::path& path,std::string_view text){
    KbmWriteResult out;
    std::error_code ec;
    if(text.size()>kKbmProfileMaxBytes){out.error="the profile would be larger than the size limit; nothing written";return out;}
    if(std::filesystem::exists(path,ec)){
        std::filesystem::path backup=path;backup+=".bak";
        if(std::filesystem::is_regular_file(backup,ec)){
            out.backupKept=true;
        }else{
            ec.clear();
            std::filesystem::copy_file(path,backup,std::filesystem::copy_options::none,ec);
            if(ec){out.error="could not keep the old profile as .bak ("+ec.message()+"); nothing written";return out;}
            out.backedUp=true;
        }
    }
    std::filesystem::path temporary=path;temporary+=".new";
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        file.write(text.data(),std::streamsize(text.size()));
        file.flush();
        if(!file){out.error="could not write the new profile";std::filesystem::remove(temporary,ec);return out;}
    }
    std::filesystem::rename(temporary,path,ec);
    if(ec){out.error="could not replace the profile ("+ec.message()+")";std::filesystem::remove(temporary,ec);return out;}
    out.ok=true;
    return out;
}
// The text to write back after a calibration: the profile read (`found`) or a new one, with the
// block merged in. Empty, with `error` set, when the file read is over the size limit: what was read
// is only its beginning, and writing that back would cut the player's file short.
inline std::optional<std::string> kbmCalibratedProfileText(std::string_view existing,bool found,const KbmProfileSet& set,
                                                           std::string_view block,std::string& error){
    error.clear();
    if(found&&existing.size()>kKbmProfileMaxBytes){
        error="the profile is larger than the size limit and is not rewritten (it would be cut short)";return std::nullopt;
    }
    const std::string fresh=found?std::string():kbmNewProfileText(set);
    std::string text=kbmMergeCalibration(found?existing:std::string_view(fresh),block);
    if(text.size()>kKbmProfileMaxBytes){error="the profile with the calibration would be larger than the size limit; not written";return std::nullopt;}
    return text;
}
// The whole text of a small file, or empty (with `found` false) when it cannot be read.
inline std::string kbmReadSmallFile(const std::filesystem::path& path,bool& found){
    found=false;
    std::ifstream file(path,std::ios::binary);
    if(!file)return {};
    std::string text;text.resize(kKbmProfileMaxBytes+1);
    file.read(text.data(),std::streamsize(text.size()));
    text.resize(std::size_t(file.gcount()));
    found=true;
    return text;
}
} // namespace veyra::remoteplay
