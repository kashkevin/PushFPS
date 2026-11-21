// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: recovery from damaged settings files.
//   slipstream.ini (veyra/engine/SlipstreamSettings.h): truncated at every byte, binary garbage,
//     too large, duplicate keys, wrong types, unknown keys, CRLF / BOM / NUL / UTF-16, and 20,000
//     seeded mutations. Never a crash, never half a file: a file with any bad line applies nothing;
//     the error is one line that never repeats the file's text.
//   The keyboard / mouse profile (veyra/remoteplay/KbmProfile.h): line by line by design (a bad line is
//     skipped and named by number), so the rule there is: every value in range, at most 32 problems
//     kept, the text never echoed, and a profile alone never presses anything.
//   The calibrated profile file (veyra/remoteplay/KbmCalibrationFile.h): the old file kept as .bak,
//     nothing written when that fails, the .new / rename step never leaves half a file, a damaged
//     calibration block replaced exactly once.
// There are no older key names in slipstream.ini to migrate (the parser has none). The Windows file
// reads (CreateFileW, QFile) and the preference JSON of the mode row (Preferences.cpp, Qt) are not
// portable and are not covered here. Running as root on Linux ignores read-only permissions, so
// failures are made with a directory in the way instead.
#include "veyra/engine/SlipstreamSettings.h"
#include "veyra/remoteplay/KbmCalibrationFile.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <filesystem>
#include <fstream>
#include <random>
namespace config_recovery_tests {
namespace se=veyra::engine;
const std::string kGoodIni=
    "# Slipstream settings\n"
    "mode = competitive\n"
    "lan = 1\n"
    "VEYRA_EXP_RP_PAD_POLL_MS = 2\n"
    "VEYRA_EXP_RP_KBM_BINDS = \"F=Square;G=Drive\"\n"
    "VEYRA_EXP_RP_ADAPTIVE_PHASE = off\n"
    "something_else = 7\n";
// Nothing at all would be applied.
bool appliesNothing(const se::SlipstreamSettings& s){return s.switches.empty()&&s.heldOff.empty()&&!s.mode&&!s.ceiling&&!s.lan;}
bool printable(std::string_view v){for(const char c:v)if(c!='\t'&&(c<0x20||c>0x7E))return false;return true;}
// What the Windows glue would do with the result is safe, whatever the text was.
void checkSafe(const se::SlipstreamSettings& s,std::string_view marker={}){
    const std::string line=se::describeSlipstreamSettings(s);
    CHECK(line.find('\n')==std::string::npos);                               // one log line
    if(!s.valid){
        CHECK(appliesNothing(s));                                            // never half a file
        CHECK(s.error.rfind("line ",0)==0||s.error=="file too large");
        CHECK(printable(s.error));
        if(!marker.empty())CHECK(s.error.find(marker)==std::string::npos);   // the line's text is never repeated
        return;
    }
    CHECK(s.unknownKeys.size()<=se::kSlipstreamMaxUnknownKeys);
    for(const auto& k:s.unknownKeys)CHECK(k.size()<=48&&printable(k));
    std::size_t fromFile=0;
    for(const auto& item:s.switches){
        // Only experiment and test switches ever reach the environment.
        CHECK(item.name.rfind("VEYRA_EXP_RP_",0)==0||item.name.rfind("VEYRA_TEST_RP_",0)==0);
        CHECK(!item.value.empty()&&item.value.size()<=se::kSlipstreamMaxValue&&printable(item.value));
        if(item.origin==se::SlipstreamOrigin::File)++fromFile;
    }
    CHECK(fromFile+s.heldOff.size()<=se::kSlipstreamMaxSwitches);
}
std::filesystem::path scratch(const char* name){
    static unsigned counter=0;
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir=std::filesystem::temp_directory_path()/("pushfps-config-recovery-"+std::to_string(stamp)+"-"+std::to_string(++counter)+"-"+name);
    std::filesystem::create_directories(dir);
    return dir;
}
std::string readAll(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
void writeAll(const std::filesystem::path& p,std::string_view text){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(text.data(),std::streamsize(text.size()));}
std::size_t count(std::string_view text,std::string_view what){std::size_t n=0;for(auto at=text.find(what);at!=std::string_view::npos;at=text.find(what,at+1))++n;return n;}
}

TEST(config_recovery_slipstream_ini_truncated_at_every_byte){
    namespace t=config_recovery_tests;
    const auto whole=t::se::parseSlipstreamSettings(t::kGoodIni);
    CHECK(whole.valid&&whole.mode&&whole.lan&&whole.unknownKeys.size()==1);
    for(std::size_t cut=0;cut<=t::kGoodIni.size();++cut){
        const auto s=t::se::parseSlipstreamSettings(std::string_view(t::kGoodIni).substr(0,cut));
        t::checkSafe(s);
        // A cut inside a quoted value can only void the file, never keep half the quote.
        for(const auto& item:s.switches)CHECK(item.value.find('"')==std::string::npos);
    }
}

TEST(config_recovery_slipstream_ini_one_bad_line_applies_nothing){
    namespace t=config_recovery_tests;
    const std::string bad[]={
        "lan = maybe\n",                       // wrong type
        "mode = fastest\n",                    // unknown mode
        "ceiling = automatic\n",               // not a concrete mode
        "[section]\n",                         // no '='
        "= 4\n",                               // no key
        "VEYRA_EXP_RP_PAD_POLL_MS = \"2\n",   // quote not closed
        "key with spaces = 1\n",
        std::string("VEYRA_EXP_RP_X = a\0b\n",21), // NUL inside a value
        "VEYRA_EXP_RP_X = caf\xC3\xA9\n",      // UTF-8 outside ASCII
        "\xEF\xBB\xBFmode = balanced\n",       // a second byte order mark, in the middle
        "VEYRA_EXP_RP_X = "+std::string(513,'a')+"\n",
    };
    for(const auto& line:bad){
        // Good lines before and after: still nothing at all.
        const auto s=t::se::parseSlipstreamSettings(t::kGoodIni+line+"VEYRA_EXP_RP_LATE = 1\n");
        CHECK(!s.valid);t::checkSafe(s);
        CHECK(s.error.rfind("line 8:",0)==0);   // the line number, never its text
    }
    // The error never repeats what the line said (it could hold anything the player pasted).
    const auto secret=t::se::parseSlipstreamSettings("VEYRA_EXP_RP_X = \"SECRET-7f3a\nmode = x\n");
    CHECK(!secret.valid);t::checkSafe(secret,"SECRET-7f3a");
    // UTF-16 (Notepad's "Unicode"): void, not misread.
    std::string utf16("\xFF\xFE",2);for(const char c:std::string("mode = balanced\r\n")){utf16+=c;utf16+='\0';}
    const auto wide=t::se::parseSlipstreamSettings(utf16);
    CHECK(!wide.valid);t::checkSafe(wide);
}

TEST(config_recovery_slipstream_ini_size_line_endings_duplicates_and_unknown_keys){
    namespace t=config_recovery_tests;
    // Too large by one byte: void. Exactly the limit, all comments: valid and empty.
    const std::string big(t::se::kSlipstreamMaxBytes+1,'#');
    CHECK(!t::se::parseSlipstreamSettings(big).valid);
    CHECK(t::se::parseSlipstreamSettings(big).error=="file too large");
    const auto limit=t::se::parseSlipstreamSettings(std::string_view(big).substr(0,t::se::kSlipstreamMaxBytes));
    CHECK(limit.valid&&t::appliesNothing(limit));
    // CRLF, a byte order mark and a missing last newline read the same as plain LF.
    std::string crlf;for(const char c:t::kGoodIni){if(c=='\n')crlf+='\r';crlf+=c;}
    const auto lf=t::se::parseSlipstreamSettings(t::kGoodIni),dos=t::se::parseSlipstreamSettings("\xEF\xBB\xBF"+crlf);
    const auto noEnd=t::se::parseSlipstreamSettings(std::string_view(t::kGoodIni).substr(0,t::kGoodIni.size()-1));
    CHECK(dos.valid&&noEnd.valid);
    CHECK(t::se::describeSlipstreamSettings(lf)==t::se::describeSlipstreamSettings(dos));
    CHECK(t::se::describeSlipstreamSettings(lf)==t::se::describeSlipstreamSettings(noEnd));
    // Empty and comment-only files apply nothing.
    for(const char* text:{"","\n\n","# only a comment","   \t\n# x\r\n"}){const auto s=t::se::parseSlipstreamSettings(text);CHECK(s.valid&&t::appliesNothing(s));}
    // Duplicates: the last one counts, for modes and for switches; an empty value holds a switch off.
    const auto dup=t::se::parseSlipstreamSettings("mode = balanced\nmode = competitive\nVEYRA_EXP_RP_PAD_POLL_MS = 1\nVEYRA_EXP_RP_PAD_POLL_MS = 3\nVEYRA_EXP_RP_ADAPTIVE_PHASE=1\nVEYRA_EXP_RP_ADAPTIVE_PHASE=\n");
    CHECK(dup.valid&&dup.mode==t::se::PushPerformanceMode::Competitive);
    std::size_t polls=0;for(const auto& item:dup.switches)if(item.name=="VEYRA_EXP_RP_PAD_POLL_MS"){++polls;CHECK(item.value=="3");}
    CHECK(polls==1);
    CHECK(std::find(dup.heldOff.begin(),dup.heldOff.end(),"VEYRA_EXP_RP_ADAPTIVE_PHASE")!=dup.heldOff.end());
    // Unknown keys never reach the environment, are listed once, at most 16, cut to 48 characters.
    std::string unknown="PATH = C:\\evil\nQT_PLUGIN_PATH = x\nVEYRA_PUSHFLOW_SYNC = 1\nveyra_exp_rp_lower = 1\nVEYRA_EXP_RP_ = 1\n";
    for(int i=0;i<40;++i)unknown+="key"+std::to_string(i%20)+std::string(60,'x')+" = 1\n";
    const auto u=t::se::parseSlipstreamSettings(unknown);
    CHECK(u.valid);t::checkSafe(u);
    CHECK(u.switches.empty()&&u.unknownKeys.size()==t::se::kSlipstreamMaxUnknownKeys);
    // Too many switches: void, not the first 64.
    std::string many;for(int i=0;i<65;++i)many+="VEYRA_EXP_RP_S"+std::to_string(i)+" = 1\n";
    const auto m=t::se::parseSlipstreamSettings(many);
    CHECK(!m.valid);t::checkSafe(m);
}

TEST(config_recovery_slipstream_ini_random_damage){
    namespace t=config_recovery_tests;
    std::mt19937 rng(20261119);
    std::uniform_int_distribution<int> byte(0,255);
    std::size_t valid=0,invalid=0;
    // Binary garbage.
    for(int i=0;i<2000;++i){
        std::string text(std::size_t(rng()%4096),'\0');
        for(auto& c:text)c=char(byte(rng));
        const auto s=t::se::parseSlipstreamSettings(text);t::checkSafe(s);(s.valid?valid:invalid)++;
    }
    // A good file damaged a little: flips, inserted NUL or newline, deleted runs, duplicated lines.
    for(int i=0;i<20000;++i){
        std::string text=t::kGoodIni;
        const int edits=1+int(rng()%4);
        for(int e=0;e<edits&&!text.empty();++e){
            const std::size_t at=rng()%text.size();
            switch(rng()%5){
            case 0:text[at]=char(byte(rng));break;
            case 1:text.insert(at,1,'\0');break;
            case 2:text.insert(at,1,'\n');break;
            case 3:text.erase(at,std::min<std::size_t>(text.size()-at,1+rng()%8));break;
            default:{const auto end=text.find('\n',at);text+=text.substr(at,end==std::string::npos?std::string::npos:end-at+1);}
            }
        }
        const auto s=t::se::parseSlipstreamSettings(text);t::checkSafe(s);(s.valid?valid:invalid)++;
    }
    CHECK(valid>0&&invalid>0);   // the damage did both
}

TEST(config_recovery_kbm_profile_damaged_text_stays_in_range_and_presses_nothing){
    namespace t=config_recovery_tests;
    const std::string good=
        "name=test\ngame=GTA 6\nall.sens_x=1.5\nwalk.deadzone=0.2\naim.curve=1.4\ndrive.mouse=steer\n"
        "walk.bind.F=Square\naim_while=L2\ncounts_per_360=12000\nall.trigger_rise_ms=40\n";
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> byte(0,255);
    const auto inRange=[](const KbmProfileSet& set){
        for(const auto& p:set.profiles){
            const auto& a=p.aim;
            for(const double v:{a.sensitivityX,a.sensitivityY,a.fullSpeedCounts,a.deadzone,a.curve,a.accel,a.accelCap,a.smoothingMs,a.carryMs,a.fineScale,a.gyroDegPerCount,
                                p.moveDeadzone,p.moveRiseMs,p.moveFallMs,p.walkTilt,p.triggerRiseMs,p.triggerFallMs,p.triggerStart,p.steerRangeCounts,p.steerReturnMs,p.steerCurve})
                if(!std::isfinite(v))return false;
            if(a.deadzone<0||a.deadzone>0.5||a.stickBits<8||a.stickBits>16)return false;
        }
        return set.name.size()<=kKbmProfileMaxName&&set.game.size()<=kKbmProfileMaxName&&std::isfinite(set.countsPer360);
    };
    for(int i=0;i<3000;++i){
        std::string text=good;
        if(i%3==0){text.assign(std::size_t(rng()%2048),'\0');for(auto& c:text)c=char(byte(rng));}
        else for(int e=0;e<1+int(rng()%5)&&!text.empty();++e){
            const std::size_t at=rng()%text.size();
            if(rng()%2)text[at]=char(byte(rng));else text.insert(at,"SECRET-9c1d"+std::string(1,char(byte(rng))));
        }
        const auto parsed=parseKbmProfiles(text);
        CHECK(inRange(parsed.set));
        CHECK(parsed.problems.size()<=kKbmProfileMaxProblems&&parsed.problemCount>=parsed.problems.size());
        for(const auto& p:parsed.problems)CHECK(p.what.find("SECRET")==std::string::npos&&p.line>=1);
        // A profile by itself never holds anything: with no key or mouse input the pad is the
        // physical pad (here at rest).
        KeyboardMousePad pad(parsed.set);ControllerState rest;rest.inputActive=true;
        ControllerState out;for(int k=0;k<3;++k)out=pad.apply(rest,4000u*unsigned(k+1));
        CHECK(out.buttons==0&&out.l2==0&&out.r2==0&&out.leftX==0&&out.leftY==0&&out.rightX==0&&out.rightY==0);
    }
    // Too long: the rest is not read, and half a line is never a setting.
    std::string longText;while(longText.size()<kKbmProfileMaxBytes+100)longText+="walk.sens_x=1.25\n";
    const auto cut=parseKbmProfiles(longText.substr(0,kKbmProfileMaxBytes+5));
    CHECK(cut.truncated&&cut.problemCount==0&&std::abs(cut.set[KbmProfileId::Walk].aim.sensitivityX-1.25)<1e-9);
    // The game name: garbage gives none (the default file), never a path.
    CHECK(kbmGameName("game=\x01\x02\n").empty());CHECK(kbmGameName("game="+std::string(65,'a')+"\n").empty());
    CHECK(kbmCalibratedFileName(kbmGameName("game=../../etc/passwd\n"))=="slipstream-kbm-etc-passwd.txt");
    CHECK(kbmCalibratedFileName("")=="slipstream-kbm-default.txt");
}

TEST(config_recovery_kbm_calibration_file_backup_and_atomic_replace){
    namespace t=config_recovery_tests;
    const auto dir=t::scratch("calibration");
    const auto file=dir/"slipstream-kbm-test.txt";
    auto bak=file;bak+=".bak";auto tmp=file;tmp+=".new";
    // A new file: no backup.
    auto w=kbmWriteProfile(file,"first\n");
    CHECK(w.ok&&!w.backedUp&&t::readAll(file)=="first\n"&&!std::filesystem::exists(tmp));
    // The next write keeps the old text as .bak.
    w=kbmWriteProfile(file,"second\n");
    CHECK(w.ok&&w.backedUp&&t::readAll(file)=="second\n"&&t::readAll(bak)=="first\n"&&!std::filesystem::exists(tmp));
    // The backup cannot be made (something else is in its place): nothing is written.
    std::filesystem::remove(bak);std::filesystem::create_directory(bak);
    w=kbmWriteProfile(file,"third\n");
    CHECK(!w.ok&&!w.error.empty()&&t::readAll(file)=="second\n"&&!std::filesystem::exists(tmp));
    std::filesystem::remove(bak);
    // The new text cannot be written: the old file stays whole.
    std::filesystem::create_directory(tmp);std::filesystem::create_directory(tmp/"keep");   // not empty: it is not removed either
    w=kbmWriteProfile(file,"fourth\n");
    CHECK(!w.ok&&t::readAll(file)=="second\n");
    std::filesystem::remove_all(tmp);
    // Reading: a missing file is "not found"; a huge one stops one byte past the limit.
    bool found=true;
    CHECK(kbmReadSmallFile(dir/"missing.txt",found).empty()&&!found);
    t::writeAll(dir/"huge.txt",std::string(kKbmProfileMaxBytes+5000,'#'));
    CHECK(kbmReadSmallFile(dir/"huge.txt",found).size()==kKbmProfileMaxBytes+1&&found);
    // Merging a calibration block: the player's lines stay, one block at the end, also when the
    // old block lost its end marker (a damaged file) and when it is merged twice.
    const std::string block=std::string(kKbmCalibrationBegin)+"\nwalk.sens_x=1.1\n"+std::string(kKbmCalibrationEnd)+"\n";
    const std::string mine="name=mine\nwalk.bind.F=Square\n";
    const auto once=kbmMergeCalibration(mine,block),twice=kbmMergeCalibration(once,block);
    CHECK(once==twice&&once.rfind(mine,0)==0&&t::count(once,kKbmCalibrationBegin)==1);
    const auto damaged=kbmMergeCalibration(mine+std::string(kKbmCalibrationBegin)+"\nwalk.sens_x=9\n",block);
    CHECK(damaged.rfind(mine,0)==0&&t::count(damaged,kKbmCalibrationBegin)==1&&damaged.find("sens_x=9")==std::string::npos);
    CHECK(kbmMergeCalibration("no newline at the end",block).rfind("no newline at the end\n",0)==0);
    std::error_code ec;std::filesystem::remove_all(dir,ec);
}
