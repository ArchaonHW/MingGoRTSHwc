#include "Campaign/Narrative/Bencao.h"
#include "Campaign/Narrative/BencaoCodex.h"
#include "Campaign/Narrative/Codex.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

} // namespace

int main() {
    const Potato::Campaign::BencaoLibrary library;
    const Potato::Campaign::BencaoCodex codex;
    const std::string page =
        Potato::Campaign::RenderCodex(library, codex);

    Check(page.find("李時珍") != std::string::npos &&
              page.find("本草綱目") != std::string::npos &&
              page.find("1596") != std::string::npos,
          "frontispiece identifies Li Shizhen and the source edition");
    Check(page.find("五十二卷、十六部") != std::string::npos &&
              page.find("八類") != std::string::npos &&
              page.find("非原書部目之轉錄") != std::string::npos,
          "frontispiece distinguishes original and game taxonomies");
    Check(page.find("勿執紙上之言以試人身") != std::string::npos,
          "frontispiece retains the historical medicine disclaimer");

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n",
                failures);
    return failures == 0 ? 0 : 1;
}
