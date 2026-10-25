"""Apply the native renderer's device feature additions to a separate build copy of Plume.

Usage: extend-plume-vulkan.py <plume_vulkan.cpp> <output> [wayland]
wayland (the Linux build): the instance also enables VK_KHR_wayland_surface when
the loader has it, for a game window on a Wayland session (Plume itself builds
for Xlib there).
"""
from pathlib import Path
import sys

p = Path(sys.argv[1])
output = Path(sys.argv[2])
wayland = sys.argv[3:] == ["wayland"]
s = p.read_text(encoding="utf-8")
assert "// Native renderer device features." not in s
s = s.replace("static const std::unordered_set<std::string> OptionalInstanceExtensions = {",
              """static const std::unordered_set<std::string> OptionalInstanceExtensions = {
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,""" + ("""
        "VK_KHR_wayland_surface",""" if wayland else ""))
s = s.replace("static const std::unordered_set<std::string> OptionalDeviceExtensions = {",
              """static const std::unordered_set<std::string> OptionalDeviceExtensions = {
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,""")
s = s.replace("        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,",
              "        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,\n        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,")
features = [
    ("dynamic", "VkPhysicalDeviceExtendedDynamicStateFeaturesEXT", "EXTENDED_DYNAMIC_STATE", "extendedDynamicState"),
    ("border", "VkPhysicalDeviceCustomBorderColorFeaturesEXT", "CUSTOM_BORDER_COLOR", "customBorderColors"),
    ("maintenance", "VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT", "SWAPCHAIN_MAINTENANCE_1", "swapchainMaintenance1"),
]
query = "// Native renderer device features.\n"
enable = ""
for name, typ, enum, field in features:
    query += f"""
        {typ} {name}Features = {{}};
        if (supportedOptionalExtensions.count(VK_EXT_{enum}_EXTENSION_NAME)) {{
            {name}Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_{enum}_FEATURES_EXT;
            {name}Features.pNext = featuresChain;
            featuresChain = &{name}Features;
        }}
"""
    enable += f"""
        if ({name}Features.{field}) {{
            {name}Features.pNext = createDeviceChain;
            createDeviceChain = &{name}Features;
        }}
"""
# The extension name has _1, whereas the structure enum has 1.
query = query.replace("supportedOptionalExtensions.count(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)",
                     "(supportedOptionalExtensions.count(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) || "
                     "supportedOptionalExtensions.count(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))")
s = s.replace("        VkPhysicalDeviceFeatures2 deviceFeatures", query + "\n        VkPhysicalDeviceFeatures2 deviceFeatures")
s = s.replace("        std::vector<const char *> enabledExtensions;", enable + "\n        std::vector<const char *> enabledExtensions;", 1)
if not output.exists() or output.read_text(encoding="utf-8") != s:
    output.write_text(s, encoding="utf-8", newline="\n")
