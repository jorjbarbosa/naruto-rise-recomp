// The family of a controller, the settings' choice over it, player one's last
// device, and the menus' hints built from them (controls/glyphs.h,
// menu/prompts.h).
#include "controls/glyphs.h"
#include "controls/service.h"
#include "menu/prompts.h"
#include "menu/text.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace narutorise::controls;
namespace {
void Check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
Device Pad(std::string key, Glyphs glyphs) {
  Device d;
  d.key = std::move(key);
  d.name = d.key;
  d.glyphs = glyphs;
  return d;
}
Device Keyboard() {
  Device d;
  d.key = "keyboard";
  d.keyboard = true;
  return d;
}
Config Automatic() {
  Config config;
  config.version = 4;
  config.ports[0].device = kAutomaticDevice;
  return config;
}
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
  return n;
}
}  // namespace

int main() {
  try {
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_PS5, 0x054c, 0x0ce6) == Glyphs::PlayStation, "DualSense");
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_PS3, 0x054c, 0x0268) == Glyphs::PlayStation, "DualShock 3");
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, 0x057e, 0x2009) == Glyphs::Nintendo, "Switch Pro");
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_STANDARD, 0x28de, 0x1205) == Glyphs::SteamDeck, "Steam Deck");
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_XBOX360, 0x045e, 0x028e) == Glyphs::Xbox, "Xbox 360");
    Check(PadGlyphs(SDL_GAMEPAD_TYPE_UNKNOWN, 0, 0) == Glyphs::Xbox, "unknown pads look like Xbox");

    Check(ForPad(Glyphs::PlayStation, "auto") == Glyphs::PlayStation, "automatic keeps the family");
    Check(ForPad(Glyphs::Xbox, "playstation") == Glyphs::PlayStation, "a choice replaces it");
    Check(ForPad(Glyphs::PlayStation, "steamdeck") == Glyphs::SteamDeck, "Steam Deck choice");
    Check(ForPad(Glyphs::PlayStation, "unknown") == Glyphs::PlayStation, "an unknown choice is automatic");

    // Automatic player one with a DualSense and the keyboard: the last used wins.
    Inventory inventory;
    inventory.devices = {Pad("sdl:ps5", Glyphs::PlayStation), Keyboard()};
    Config config = Automatic();
    ActiveInput input;
    Check(input.Current(inventory, config, "auto") == Glyphs::PlayStation, "the pad before any input");
    input.ObserveKeyboard(config);
    Check(input.Current(inventory, config, "auto") == Glyphs::Keyboard, "a key takes over");
    input.ObservePads(inventory, config);
    Check(input.Current(inventory, config, "auto") == Glyphs::Keyboard, "an idle pad does not");
    inventory.devices[0].raw.axes[0] = .3;  // drift
    input.ObservePads(inventory, config);
    Check(input.Current(inventory, config, "auto") == Glyphs::Keyboard, "drift does not");
    inventory.devices[0].raw.buttons = 0x1000;
    input.ObservePads(inventory, config);
    Check(input.Current(inventory, config, "auto") == Glyphs::PlayStation, "a press takes back");
    Check(input.Current(inventory, config, "xbox") == Glyphs::Xbox, "the choice applies to the pad");
    // A held button does not take over again after the keyboard.
    input.ObserveKeyboard(config);
    input.ObservePads(inventory, config);
    Check(input.Current(inventory, config, "auto") == Glyphs::Keyboard, "a held press stays behind");
    inventory.devices[0].raw.buttons = 0;
    input.ObservePads(inventory, config);
    inventory.devices[0].raw.buttons = 0x2000;
    input.ObservePads(inventory, config);
    Check(input.Current(inventory, config, "auto") == Glyphs::PlayStation, "a new press takes over");

    // A small mouse nudge does not take over; a deliberate motion does.
    const auto now = std::chrono::steady_clock::now();
    input.ObserveMouseMotion(3, 2, config, now);
    Check(input.Current(inventory, config, "auto") == Glyphs::PlayStation, "a nudge does not");
    input.ObserveMouseMotion(12, 9, config, now + std::chrono::milliseconds(100));
    Check(input.Current(inventory, config, "auto") == Glyphs::Keyboard, "motion does");

    // A pad assigned to player one: the keyboard plays for nobody there.
    Config pad_only = Automatic();
    pad_only.ports[0].device = "sdl:ps5";
    ActiveInput assigned;
    assigned.ObserveKeyboard(pad_only);
    Check(assigned.Current(inventory, pad_only, "auto") == Glyphs::PlayStation, "keys of no player");
    // Keyboard alone: its keys.
    Inventory keyboard_only;
    keyboard_only.devices = {Keyboard()};
    Check(ActiveInput().Current(keyboard_only, Automatic(), "auto") == Glyphs::Keyboard, "keyboard alone");

    // The game's icons: the physical input the profile maps to each guest input.
    Port port;
    Check(GlyphSprite("south", Glyphs::Xbox, port) == "xbox-south", "A on Xbox");
    Check(GlyphSprite("south", Glyphs::PlayStation, port) == "ps-south", "A is the cross");
    Check(GlyphSprite("lt", Glyphs::Nintendo, port) == "switch-lt", "LT is ZL");
    Check(GlyphSprite("dpad", Glyphs::Xbox, port) == "xbox-dpad", "the whole D-pad");
    Check(GlyphSprite("lstick", Glyphs::SteamDeck, port) == "deck-lstick", "the whole left stick");
    port.pad[0] = {SourceKind::Button, 0x2000};  // A on the East button
    Check(GlyphSprite("south", Glyphs::PlayStation, port) == "ps-east", "a remapped A shows the circle");
    port.pad[6] = {SourceKind::Axis, 1, 1};  // LT on the left stick up
    Check(GlyphSprite("lt", Glyphs::Xbox, port) == "xbox-lstick-up", "a trigger moved to a stick");
    port.pad[1] = {};
    Check(GlyphSprite("east", Glyphs::Xbox, port).empty(), "an unbound input keeps the game's icon");
    Check(GlyphSprite("south", Glyphs::Keyboard, port) == "key-space", "A is Space");
    Check(GlyphSprite("west", Glyphs::Keyboard, port) == "key-lmb", "X is the left mouse button");
    Check(GlyphSprite("rstick", Glyphs::Keyboard, port) == "key-mouse", "the mouse turns the camera");
    port.mouse = false;
    Check(GlyphSprite("rstick", Glyphs::Keyboard, port) == "key-up", "without the mouse, the arrows");
    Check(GlyphSprite("lstick", Glyphs::Keyboard, port) == "key-w", "the left stick is WASD");
    port.keys[0] = ",Q";
    Check(GlyphSprite("south", Glyphs::Keyboard, port) == "key-q", "the first bound alternative");
    Check(GlyphSprite("unknown", Glyphs::Xbox, port).empty(), "unknown inputs");

    // Hints: the menus' actions become the family's glyphs.
    using narutorise::menu::PromptRml;
    const auto pad = PromptRml("{confirm}  Select     {tabs}  Tabs", Glyphs::PlayStation);
    Check(Count(pad, "sprite='ps-south'") == 1 && Count(pad, "sprite='ps-lb'") == 1 &&
          Count(pad, "sprite='ps-rb'") == 1 && pad.find("  Select     ") != std::string::npos, "pad hints");
    const auto keys = PromptRml("{back}  Back &", Glyphs::Keyboard);
    Check(Count(keys, "sprite='key-escape'") == 1 && keys.find("&amp;") != std::string::npos, "key hints, escaped");
    Check(PromptRml("{unknown} x", Glyphs::Xbox).find("{unknown}") != std::string::npos, "unknown tags stay text");
    Check(PromptRml("{preview}", Glyphs::Keyboard).empty(), "no keyboard glyph for the sticks");
    for (const char* hint : {"setup-hints", "mapping-hints", "list-hints", "close-hint", "panel-hints", "c-hints",
                             "c-preview-hints"}) {
      const std::string text = narutorise::menu::Text(hint);
      Check(!text.empty() && text.find('{') != std::string::npos, hint);
      Check(PromptRml(text, Glyphs::Xbox).find('{') == std::string::npos, hint);
    }
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "controls glyphs ok\n";
  return 0;
}
