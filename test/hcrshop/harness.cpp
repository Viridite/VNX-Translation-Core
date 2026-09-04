// Host test harness for Hill Climb Racing's shop catalogue replay.
//
// This is 69 calls into the game with twelve arguments each, made once, before
// the engine builds its shop. On hardware a mistake in them is not a crash —
// it is a wrong price or a missing product on a screen nobody screenshots, so
// it is checked here instead.
//
//   g++ -std=c++17 -I test/arm32/mock -I include test/hcrshop/harness.cpp source/compat/games/game_hillclimb_shop.cpp -o /tmp/hcrshoptest && /tmp/hcrshoptest
#include "compat/games.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

void compatLog(const char* m) { printf("  [log] %s\n", m); }
void compatLogFmt(const char* f, ...) {
    va_list a; va_start(a, f); printf("  [log] "); vprintf(f, a); printf("\n"); va_end(a);
}

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

// What the game would have been told.
struct Item {
    std::string id, price, amount, bonus, icon;
    int ad_free, unknown, special, gems, paints, bundle;
};
static std::vector<Item> g_items;
static std::vector<std::pair<std::string, std::string>> g_prices, g_currencies;
static void* g_lastEnv = nullptr;

extern "C" {
static void fake_setInAppItem(void* env, void*, const char* id, const char* price,
                              const char* amount, const char* bonus, const char* icon,
                              unsigned char ad_free, unsigned char unknown,
                              unsigned char special, unsigned char gems,
                              unsigned char paints, int bundle) {
    g_lastEnv = env;
    g_items.push_back({id, price, amount, bonus, icon,
                       ad_free, unknown, special, gems, paints, bundle});
}
static void fake_setPrice(void*, void*, const char* id, const char* price) {
    g_prices.push_back({id, price});
}
static void fake_setCurrency(void*, void*, const char* id, const char* cur) {
    g_currencies.push_back({id, cur});
}
}

// Stands in for the game's export table.
static bool g_haveShopNatives = true;
static void* resolver(const char* sym) {
    if (!g_haveShopNatives) return nullptr;
    if (!strcmp(sym, "Java_com_fingersoft_game_MainActivity_setInAppItem"))
        return (void*)fake_setInAppItem;
    if (!strcmp(sym, "Java_com_fingersoft_game_MainActivity_setInAppItemPrice"))
        return (void*)fake_setPrice;
    if (!strcmp(sym, "Java_com_fingersoft_game_MainActivity_setInAppItemCurrencyCode"))
        return (void*)fake_setCurrency;
    return nullptr;
}

int main() {
    printf("hcrshop\n");

    // ── The amount formatter ────────────────────────────────────────────────
    // Every price in the shop goes through this.
    char b[24];
    hcrFormatShopAmount(b, 1000000);
    check(!strcmp(b, "1 000 000"), "a million formats as \"1 000 000\"");
    hcrFormatShopAmount(b, 100);        check(!strcmp(b, "100"), "three digits are left alone");
    hcrFormatShopAmount(b, 1000);       check(!strcmp(b, "1 000"), "four digits get one separator");
    hcrFormatShopAmount(b, 0);          check(!strcmp(b, "0"), "zero formats as zero");
    hcrFormatShopAmount(b, 160000000);  check(!strcmp(b, "160 000 000"), "the largest catalogue amount fits");

    // ── The catalogue ───────────────────────────────────────────────────────
    check(hcrShopCatalogCount() == 69, "the catalogue is the 69 products Init() sets");
    bool dupes = false, empty = false;
    for (size_t i = 0; i < hcrShopCatalogCount(); i++) {
        const char* a = hcrShopCatalogId(i);
        if (!a || !*a) { empty = true; continue; }
        if (strncmp(a, "com.fingersoft.hillclimb", 24) != 0) empty = true;
        for (size_t j = i + 1; j < hcrShopCatalogCount(); j++)
            if (!strcmp(a, hcrShopCatalogId(j))) dupes = true;
    }
    check(!empty, "every product has a Fingersoft product id");
    check(!dupes, "no product is listed twice");
    check(hcrShopCatalogId(69) == nullptr, "reading past the end returns nothing");

    // ── The replay ──────────────────────────────────────────────────────────
    int env = 0, thiz = 0;
    bool ok = hcrPopulateShop(&env, &thiz, resolver);
    check(ok, "the replay reports success when the game exports the natives");
    check(g_items.size() == 69, "the game is told about all 69 products");
    check(g_lastEnv == &env, "...with the JNI env it was given");
    check(g_prices.size() == 69 && g_currencies.size() == 69,
          "every product also gets a price and a currency");

    bool allFree = true, allPriced = true;
    for (const Item& it : g_items) if (it.price != "FREE") allFree = false;
    for (auto& p : g_prices) if (p.second != "FREE") allPriced = false;
    check(allFree && allPriced,
          "every product is priced FREE — there is no billing backend to charge through");

    // Spot-check the shape of a few entries against the real catalogue: a
    // coin pack, a gem pack, a paint, and a bundle each take a different path
    // through the game's own reward handling.
    auto find = [](const char* id) -> const Item* {
        for (const Item& it : g_items) if (it.id == id) return &it;
        return nullptr;
    };
    const Item* coins = find("com.fingersoft.hillclimb.adfree_2000000coins");
    check(coins && coins->amount == "2 000 000" && coins->icon == "coin3" &&
          coins->bonus == "+166%" && coins->gems == 0 && coins->bundle == 0,
          "a coin pack carries its formatted amount, icon and bonus badge");
    const Item* gems = find("com.fingersoft.hillclimb.iap2.adfree_500gems");
    check(gems && gems->gems == 1 && gems->amount == "500" && gems->icon == "diamond1",
          "a gem pack is flagged as gems");
    const Item* paint = find("com.fingersoft.hillclimb.paint2");
    check(paint && paint->paints == 1 && paint->amount == "160",
          "a paint is flagged as paint");
    const Item* bundle = find("com.fingersoft.hillclimb.bundle21");
    check(bundle && bundle->bundle == 21 && bundle->amount == "0",
          "a bundle carries its bundle id");
    const Item* garage = find("com.fingersoft.hillclimb.special_garage1");
    check(garage && garage->bundle == 1 && garage->gems == 1,
          "the garage bundles keep both flags they are set with");

    // ── A build that can't be given a catalogue ─────────────────────────────
    g_items.clear(); g_prices.clear(); g_currencies.clear();
    g_haveShopNatives = false;
    check(!hcrPopulateShop(&env, &thiz, resolver),
          "a build not exporting setInAppItem reports failure, so the patches stay");
    check(g_items.empty(), "...and nothing is called");
    check(!hcrPopulateShop(&env, &thiz, nullptr), "a null resolver is handled");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
