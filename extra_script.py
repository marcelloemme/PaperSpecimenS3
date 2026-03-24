"""
Pre-build script: ensures FreeType CFF/PostScript modules are available.
Copies modules from freetype_extra/ into the OpenFontRender library if missing,
and writes the correct ftmodule.h with proper initialization order.
"""
import os
import shutil

Import("env")

# Correct module order: dependencies (psnames, psaux, pshinter) BEFORE cff
FTMODULE_CONTENT = """/*
 *  This file registers the FreeType modules compiled into the library.
 *  Patched by extra_script.py to enable CFF/OTF support.
 *  Order matters: psnames/psaux/pshinter must come before cff.
 */

//FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, t1cid_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, pfr_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, t42_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, winfnt_driver_class )
//FT_USE_MODULE( FT_Driver_ClassRec, pcf_driver_class )
//FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
//FT_USE_MODULE( FT_Renderer_Class, ft_smooth_lcd_renderer_class )
//FT_USE_MODULE( FT_Renderer_Class, ft_smooth_lcdv_renderer_class )
//FT_USE_MODULE( FT_Driver_ClassRec, bdf_driver_class )

/* EOF */
"""

def patch_freetype(source, target, env):
    # Find OpenFontRender in .pio/libdeps
    project_dir = env.get("PROJECT_DIR", "")
    ofr_src = os.path.join(project_dir, ".pio", "libdeps", "m5papers3",
                           "OpenFontRender", "src")

    if not os.path.isdir(ofr_src):
        print("WARNING: OpenFontRender not found, skipping CFF patch")
        return

    extra_dir = os.path.join(project_dir, "freetype_extra")
    if not os.path.isdir(extra_dir):
        print("WARNING: freetype_extra/ not found, skipping CFF patch")
        return

    # Copy module directories if missing
    modules = ["cff", "psaux", "pshinter", "psnames"]
    copied = False
    for mod in modules:
        src = os.path.join(extra_dir, mod)
        dst = os.path.join(ofr_src, mod)
        if os.path.isdir(src) and not os.path.isdir(dst):
            shutil.copytree(src, dst)
            print(f"  Copied FreeType module: {mod}")
            copied = True

    # Always write the correct ftmodule.h (with proper order)
    ftmod_path = os.path.join(ofr_src, "freetype", "config", "ftmodule.h")
    if os.path.isfile(ftmod_path):
        with open(ftmod_path, "r") as f:
            current = f.read()

        if "Patched by extra_script.py" not in current:
            with open(ftmod_path, "w") as f:
                f.write(FTMODULE_CONTENT)
            print("  Patched ftmodule.h: enabled CFF with correct module order")

    # Patch ftoption.h to enable PostScript names and Adobe Glyph List
    ftoption_path = os.path.join(ofr_src, "freetype", "config", "ftoption.h")
    if os.path.isfile(ftoption_path):
        with open(ftoption_path, "r") as f:
            content = f.read()

        changed = False
        if "//#define FT_CONFIG_OPTION_POSTSCRIPT_NAMES" in content:
            content = content.replace(
                "//#define FT_CONFIG_OPTION_POSTSCRIPT_NAMES",
                "#define FT_CONFIG_OPTION_POSTSCRIPT_NAMES")
            changed = True
        if "//#define FT_CONFIG_OPTION_ADOBE_GLYPH_LIST" in content:
            content = content.replace(
                "//#define FT_CONFIG_OPTION_ADOBE_GLYPH_LIST",
                "#define FT_CONFIG_OPTION_ADOBE_GLYPH_LIST")
            changed = True

        if changed:
            with open(ftoption_path, "w") as f:
                f.write(content)
            print("  Patched ftoption.h: enabled POSTSCRIPT_NAMES + ADOBE_GLYPH_LIST")

    if copied:
        print("FreeType CFF support installed successfully")

# Run immediately at script load time (before any compilation)
patch_freetype(None, None, env)
