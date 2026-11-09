import sys
p = "/usr/local/src/mesa-26.2.3/src/gallium/drivers/zink/zink_screen.c"
s = open(p).read()
old = """      if (drm_props.renderMajor == dev_major &&
          drm_props.renderMinor == dev_minor)
         return i;
   }

   return -1;
}"""
new = """      if (drm_props.renderMajor == dev_major &&
          drm_props.renderMinor == dev_minor)
         return i;
   }

   /* lsw: Turnip on KGSL has no DRM node, so nothing matches; use the first real GPU */
   for (uint32_t i = 0; i < pdev_count; ++i) {
      VKSCR(GetPhysicalDeviceProperties2)(pdevs[i], &props);
      if (props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
         mesa_logw("ZINK: no pdev matches DRM %d:%d, using %s", (int)dev_major, (int)dev_minor,
                   props.properties.deviceName);
         return i;
      }
   }

   return -1;
}"""
if "lsw: Turnip on KGSL" in s: print("already patched"); sys.exit(0)
assert s.count(old) == 1, "pattern not found"
open(p, "w").write(s.replace(old, new)); print("patched")
