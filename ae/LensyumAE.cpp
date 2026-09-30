// Lensyum for After Effects: SmartFX wrapper around the lensyum_core optics engine.
// 8, 16 and 32 bpc, multi-frame rendering safe (the only shared state is the PSF cache,
// which has its own lock).

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
#include "adobesdk/DrawbotSuite.h"
#include "Param_Utils.h"
#include "SPBasic.h"

#include "LensyumParams.h"
#include "lensyum/DepthAI.h"
#include "lensyum/Renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifdef AE_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace lensyum;

namespace {

// ------------------------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------------------------

const char* kFormatNames = "Full Frame (36 mm)|Super 35 (24.89 mm)|APS-C (23.6 mm)|Micro Four Thirds (17.3 mm)|Super 16 (12.52 mm)|Large Format 65 (54.12 mm)|Custom";
const double kFormatWidths[] = {36.0, 24.89, 23.6, 17.3, 12.52, 54.12};
constexpr int kFormatCount = 7;
constexpr int kBannerUiWidth = 200;  // hint only: the banner is drawn across the whole row
constexpr int kBannerUiHeight = 150;

void logLine(const std::string& text);
void logStep(const char* what);

const std::string& lensPopupString() {
    static const std::string s = [] {
        std::string r;
        for (int i = 0; i < lensPresetCount(); ++i) {
            if (i) r += "|";
            r += lensPreset(i).name;
        }
        return r;
    }();
    return s;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_ParamDef def;

#ifdef LENSYUM_BANNER
    // Banner: a parameter without data whose only job is to be drawn (see DrawBanner).
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_NO_DATA;
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.ui_flags = PF_PUI_CONTROL;
    def.ui_width = kBannerUiWidth;
    def.ui_height = kBannerUiHeight;
    PF_STRCPY(def.PF_DEF_NAME, " ");
    def.uu.id = ID_BANNER;
    if (const PF_Err e = (*in_data->inter.add_param)(in_data->effect_ref, -1, &def)) return e;
    // A custom UI must be registered, as in the SDK's Custom_ECW_UI sample; without this After
    // Effects crashes when it builds the panel. Only the Effect Controls panel is used.
    {
        PF_CustomUIInfo ci;
        AEFX_CLR_STRUCT(ci);
        ci.events = PF_CustomEFlag_EFFECT;
        ci.comp_ui_width = ci.comp_ui_height = 0;
        ci.comp_ui_alignment = PF_UIAlignment_NONE;
        ci.layer_ui_width = ci.layer_ui_height = 0;
        ci.layer_ui_alignment = PF_UIAlignment_NONE;
        ci.preview_ui_width = ci.preview_ui_height = 0;
        ci.preview_ui_alignment = PF_UIAlignment_NONE;
        if (const PF_Err e = (*in_data->inter.register_ui)(in_data->effect_ref, &ci)) return e;
    }
#endif

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Camera", ID_CAMERA_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Lens Preset", lensPresetCount(), 1, lensPopupString().c_str(), ID_LENS_PRESET);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Format", kFormatCount, 1, kFormatNames, ID_FORMAT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sensor Width (Format: Custom)", 2, 100, 5, 70, 36, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SENSOR_WIDTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("F-Stop", 0.7, 64, 0.7, 22, 2.8, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_FSTOP);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Filmback Offset (mm)", -20, 20, -1, 1, 0, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_FILMBACK_OFFSET);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lens Coverage", 0, 3, 0, 3, 1, PF_Precision_THOUSANDTHS, PF_ValueDisplayFlag_NONE, 0, ID_COVERAGE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_CAMERA_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Focus", ID_FOCUS_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Defocus Source", 4, 1, "Whole Frame|Depth Map|Focus Region|AI Depth (auto)", ID_DEFOCUS_MODE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Show Depth Map", FALSE, 0, ID_SHOW_DEPTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Show 3D Focus View", FALSE, 0, ID_SHOW_3D);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("3D Focus View", ID_3D_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Orbit", -85, 85, -85, 85, 30, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_3D_YAW);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Tilt", -85, 85, -85, 85, 20, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_3D_PITCH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Relief", 0, 300, 0, 200, 60, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_3D_RELIEF);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_3D_TOPIC_END);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Defocus Amount (px)", -500, 500, -150, 150, 30, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_DEFOCUS_AMOUNT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Focus Point", 50, 50, FALSE, ID_FOCUS_POINT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Focus Distance (m)", 0.05, 100000, 0.2, 30, 3, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_FOCUS_DISTANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Defocus Scale", 0, 1000, 0, 300, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_DEFOCUS_SCALE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Edge Clean-up (px)", 0, 12, 0, 12, 3, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_EDGE_CLEAN);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Focus Region", ID_REGION_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Region Radius (px)", 0, 10000, 0, 1500, 300, PF_Precision_INTEGER, PF_ValueDisplayFlag_NONE, 0, ID_REGION_RADIUS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Region Falloff (px)", 1, 10000, 1, 2000, 400, PF_Precision_INTEGER, PF_ValueDisplayFlag_NONE, 0, ID_REGION_FALLOFF);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Region Aspect", 0.1, 10, 0.25, 4, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_REGION_ASPECT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Keep Center Untouched", TRUE, 0, ID_REGION_KEEP);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_REGION_TOPIC_END);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Depth Map", ID_DEPTH_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Depth Layer", PF_LayerDefault_NONE, ID_DEPTH_LAYER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Depth Polarity", 2, 1, "White Is Near|White Is Far", ID_DEPTH_WHITE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Depth Encoding", 2, 2, "Distance (linear)|Disparity (1/z)", ID_DEPTH_ENCODING);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Near Distance (m)", 0.05, 1000, 0.1, 10, 0.5, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_DEPTH_NEAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Far Distance (m)", 0.1, 100000, 1, 200, 50, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_DEPTH_FAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Focus On Point", FALSE, 0, ID_FOCUS_PICK);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_DEPTH_TOPIC_END);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("AI Depth", ID_AI_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Depth Detail", 4, 2, "Low|Medium|High|Ultra", ID_AI_DETAIL);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Edge Refine", TRUE, 0, ID_AI_REFINE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Use GPU", TRUE, 0, ID_AI_GPU);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Focus On Focus Point", TRUE, 0, ID_AI_FOCUS_POINT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sharp Range", 0, 50, 0, 50, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_AI_RANGE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Far Cut", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_AI_FAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Near Cut", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_AI_NEAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Contrast", 0.2, 5, 0.2, 5, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_AI_CONTRAST);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Shift", -50, 50, -50, 50, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_AI_SHIFT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Smooth (px)", 0, 100, 0, 60, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_AI_SMOOTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Invert Depth", FALSE, 0, ID_AI_INVERT);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_AI_TOPIC_END);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_FOCUS_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Aperture", ID_APERTURE_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Shape", 8, 1, "Iris (Blades)|Heart|Star|Triangle|Diamond|Cross|Ring|Crescent", ID_APERTURE_SHAPE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blades", 0, 24, 0, 16, 0, ID_BLADES);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blade Curvature", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_BLADE_CURVATURE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Blade Rotation", 0, ID_BLADE_ROTATION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Central Obstruction", 0, 90, 0, 90, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_OBSTRUCTION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Obstruction Softness", 0, 100, 0, 100, 50, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_OBSTRUCTION_SOFT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Custom Aperture", PF_LayerDefault_NONE, ID_CUSTOM_APERTURE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_APERTURE_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Creative", ID_CREATIVE_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Impression", -100, 100, -100, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_IMPRESSION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Impression Power", 0.5, 10, 0.5, 8, 3, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_IMPRESSION_POWER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Coma", -100, 100, -100, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_COMA);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Astigmatism (mm)", -10, 10, -3, 3, 0, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_ASTIGMATISM);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Field Curvature (mm)", -10, 10, -2, 2, 0, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_FIELD_CURVATURE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Zonal Ripple", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_ZONAL_RIPPLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Ripple Density", 1, 30, 1, 20, 6, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_RIPPLE_DENSITY);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lobes", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_LOBES);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Lobe Count", 2, 12, 2, 12, 3, ID_LOBE_COUNT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lobe Power", 0.2, 10, 0.2, 8, 3, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_LOBE_POWER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Lobe Angle", 0, ID_LOBE_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Lobes Face Center", FALSE, 0, ID_LOBES_FACE_CENTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Chromatic Aberration", TRUE, 0, ID_CA_ENABLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Bokeh Fringing", 0, 1000, 0, 500, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CHROMATIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lateral CA (px)", -20, 20, -5, 5, 0.5, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_LATERAL_CA);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Cat-Eye", 0, 200, 0, 200, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CATEYE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Anamorphic Squeeze", 1, 3, 1, 2.5, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SQUEEZE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Bokeh Imperfections", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_TEXTURE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Texture Scale", 10, 400, 25, 300, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_TEXTURE_SCALE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Texture Seed", 0, 10000, 0, 100, 0, ID_TEXTURE_SEED);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_CREATIVE_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Layers", ID_LAYERS_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Rain", FALSE, 0, ID_RAIN_ENABLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Rain Layer", PF_LayerDefault_NONE, ID_RAIN_LAYER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Rain Distance (m)", 0.01, 1000, 0.05, 10, 0.3, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_RAIN_DISTANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Rain Strength", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_RAIN_STRENGTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Rain Refraction (px)", 0, 100, 0, 40, 8, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_RAIN_REFRACTION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shimmer", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_SHIMMER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shimmer Density", 0.2, 5, 0.2, 5, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SHIMMER_DENSITY);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Shimmer Seed", 0, 10000, 0, 100, 0, ID_SHIMMER_SEED);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_LAYERS_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Highlights", ID_HIGHLIGHT_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold", 0, 100, 0, 100, 85, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_HI_THRESHOLD);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Boost", 0, 500, 0, 50, 2, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_HI_GAIN);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_HIGHLIGHT_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Render", ID_RENDER_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blend Back", 0, 1, 0, 1, 1, PF_Precision_THOUSANDTHS, PF_ValueDisplayFlag_NONE, 0, ID_BLEND_BACK);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Quality", 4, 2, "Draft|Normal|High|Best", ID_QUALITY);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Max Blur Radius (px)", 4, 1000, 16, 400, 150, PF_Precision_INTEGER, PF_ValueDisplayFlag_NONE, 0, ID_MAX_BLUR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Depth Slices", 1, 48, 1, 32, 12, ID_LAYERS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Working Space", 3, 1, "Auto|Linear|sRGB Display", ID_COLOR_MODE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Optical Center", 50, 50, FALSE, ID_OPTICAL_CENTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("View", 5, 1, "Result|Blur Map|Bokeh Grid|Depth Map|Focus Overlay", ID_VIEW);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_RENDER_TOPIC_END);

    out_data->num_params = P_COUNT;
    logStep("ParamsSetup done");
    return PF_Err_NONE;
}

// Checks a parameter out, copies the value and checks it back in.
class ParamReader {
public:
    explicit ParamReader(PF_InData* in) : in_(in) {}
    bool ok() const { return err_ == PF_Err_NONE; }
    PF_Err err() const { return err_; }

    double num(int index) {
        PF_ParamDef p;
        if (!checkout(index, p)) return 0;
        double v = 0;
        switch (p.param_type) {
        case PF_Param_FLOAT_SLIDER: v = p.u.fs_d.value; break;
        case PF_Param_SLIDER: v = p.u.sd.value; break;
        case PF_Param_POPUP: v = p.u.pd.value; break;
        case PF_Param_CHECKBOX: v = p.u.bd.value; break;
        case PF_Param_ANGLE: v = FIX_2_FLOAT(p.u.ad.value); break;
        default: break;
        }
        checkin(p);
        return v;
    }
    void point(int index, double& x, double& y) {
        PF_ParamDef p;
        x = y = 0;
        if (!checkout(index, p)) return;
        x = FIX_2_FLOAT(p.u.td.x_value);
        y = FIX_2_FLOAT(p.u.td.y_value);
        checkin(p);
    }

private:
    bool checkout(int index, PF_ParamDef& p) {
        AEFX_CLR_STRUCT(p);
        if (err_ != PF_Err_NONE) return false;
        err_ = PF_CHECKOUT_PARAM(in_, index, in_->current_time, in_->time_step, in_->time_scale, &p);
        return err_ == PF_Err_NONE;
    }
    void checkin(PF_ParamDef& p) {
        const PF_Err e = PF_CHECKIN_PARAM(in_, &p);
        if (err_ == PF_Err_NONE) err_ = e;
    }
    PF_InData* in_;
    PF_Err err_ = PF_Err_NONE;
};

// ------------------------------------------------------------------------------------
// Pixel conversion
// ------------------------------------------------------------------------------------

inline float srgbDecode(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float srgbEncode(float v) {
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

struct WorldView {
    PF_EffectWorld* w = nullptr;
    PF_PixelFormat fmt = PF_PixelFormat_ARGB32;
    int width() const { return w ? w->width : 0; }
    int height() const { return w ? w->height : 0; }
    char* row(int y) const { return reinterpret_cast<char*>(w->data) + static_cast<size_t>(y) * w->rowbytes; }

    // Straight read into premultiplied float ARGB -> RGBA. AE worlds are premultiplied already.
    void read(int x, int y, float* rgba) const {
        const char* r = row(y);
        switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            const PF_PixelFloat& p = reinterpret_cast<const PF_PixelFloat*>(r)[x];
            rgba[0] = p.red; rgba[1] = p.green; rgba[2] = p.blue; rgba[3] = p.alpha;
            break;
        }
        case PF_PixelFormat_ARGB64: {
            const PF_Pixel16& p = reinterpret_cast<const PF_Pixel16*>(r)[x];
            const float k = 1.0f / PF_MAX_CHAN16;
            rgba[0] = p.red * k; rgba[1] = p.green * k; rgba[2] = p.blue * k; rgba[3] = p.alpha * k;
            break;
        }
        default: {
            const PF_Pixel8& p = reinterpret_cast<const PF_Pixel8*>(r)[x];
            const float k = 1.0f / PF_MAX_CHAN8;
            rgba[0] = p.red * k; rgba[1] = p.green * k; rgba[2] = p.blue * k; rgba[3] = p.alpha * k;
            break;
        }
        }
    }
    void write(int x, int y, const float* rgba) const {
        char* r = row(y);
        switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            PF_PixelFloat& p = reinterpret_cast<PF_PixelFloat*>(r)[x];
            p.red = rgba[0]; p.green = rgba[1]; p.blue = rgba[2]; p.alpha = rgba[3];
            break;
        }
        case PF_PixelFormat_ARGB64: {
            PF_Pixel16& p = reinterpret_cast<PF_Pixel16*>(r)[x];
            auto q = [](float v) { return static_cast<A_u_short>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * PF_MAX_CHAN16)); };
            p.red = q(rgba[0]); p.green = q(rgba[1]); p.blue = q(rgba[2]); p.alpha = q(rgba[3]);
            break;
        }
        default: {
            PF_Pixel8& p = reinterpret_cast<PF_Pixel8*>(r)[x];
            auto q = [](float v) { return static_cast<A_u_char>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * PF_MAX_CHAN8)); };
            p.red = q(rgba[0]); p.green = q(rgba[1]); p.blue = q(rgba[2]); p.alpha = q(rgba[3]);
            break;
        }
        }
    }
};

PF_Err pixelFormat(PF_InData* in_data, PF_EffectWorld* world, PF_PixelFormat& fmt) {
    SPBasicSuite* sp = in_data->pica_basicP;
    const void* suite = nullptr;
    if (!sp || sp->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, &suite) != kSPNoError || !suite)
        return PF_Err_BAD_CALLBACK_PARAM;
    const PF_WorldSuite2* ws = static_cast<const PF_WorldSuite2*>(suite);
    const PF_Err err = ws->PF_GetPixelFormat(world, &fmt);
    sp->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    return err;
}

// Luminance of a world, resampled to at most maxSide on the long edge (for masks and depth).
void worldLuminance(const WorldView& v, int maxSide, bool useAlpha, std::vector<float>& out, int& ow, int& oh) {
    const int w = v.width(), h = v.height();
    const double scale = std::min(1.0, maxSide / double(std::max(w, h)));
    ow = std::max(2, static_cast<int>(std::lround(w * scale)));
    oh = std::max(2, static_cast<int>(std::lround(h * scale)));
    out.assign(static_cast<size_t>(ow) * oh, 0.0f);
    for (int y = 0; y < oh; ++y) {
        const int sy = std::min(h - 1, static_cast<int>((y + 0.5) * h / oh));
        for (int x = 0; x < ow; ++x) {
            const int sx = std::min(w - 1, static_cast<int>((x + 0.5) * w / ow));
            float p[4];
            v.read(sx, sy, p);
            const float a = p[3];
            float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (!useAlpha && a > 0) l /= a; // un-premultiply depth values
            out[static_cast<size_t>(y) * ow + x] = l;
        }
    }
}

// ------------------------------------------------------------------------------------
// Smart render
// ------------------------------------------------------------------------------------

struct PreRenderData {
    PF_LRect inRect;
    PF_LRect outRect;
    double layerW, layerH;
    double par;
};

void deletePreRenderData(void* p) { delete static_cast<PreRenderData*>(p); }

// Folder the .aex lives in, with a trailing separator. AI depth files sit next to it.
std::string pluginFolder() {
#ifdef AE_OS_WIN
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&pluginFolder), &self))
        return std::string();
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH * 2);
    std::wstring path(buf, n);
    const size_t cut = path.find_last_of(L"\\/");
    path = cut == std::wstring::npos ? std::wstring() : path.substr(0, cut + 1);
    const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &out[0], len, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
#else
    return std::string();
#endif
}

void logLine(const std::string& text) {
#ifdef AE_OS_WIN
    char tmp[MAX_PATH + 1] = {0};
    if (GetTempPathA(MAX_PATH, tmp) > 0) {
        if (FILE* f = std::fopen((std::string(tmp) + "lensyum_log.txt").c_str(), "a")) {
            std::fprintf(f, "%s\n", text.c_str());
            std::fclose(f);
        }
    }
#else
    (void)text;
#endif
}

// Progress marks for the first calls of each process, so a crash can be placed from the log.
void logStep(const char* what) {
    static std::atomic<int> count{0};
    if (count.fetch_add(1) < 160) logLine(what);
}

#ifdef LENSYUM_BANNER
// ------------------------------------------------------------------------------------
// Banner at the top of the Effect Controls panel
// ------------------------------------------------------------------------------------

// Raw BGRA picture embedded as the LZ_BANNER_IMAGE resource (not named after the LENSYUM_BANNER define, which the resource compiler would substitute): width and height (uint32 LE), pixels.
struct BannerImage {
    int w = 0, h = 0;
    const unsigned char* bgra = nullptr;
};

const BannerImage& bannerImage() {
    static const BannerImage img = [] {
        BannerImage r;
#ifdef AE_OS_WIN
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&pluginFolder), &self)) {
            if (HRSRC res = FindResourceW(self, L"LZ_BANNER_IMAGE", MAKEINTRESOURCEW(10)) /* RT_RCDATA */) {
                const DWORD n = SizeofResource(self, res);
                HGLOBAL mem = LoadResource(self, res);
                const unsigned char* p = mem ? static_cast<const unsigned char*>(LockResource(mem)) : nullptr;
                if (p && n > 8) {
                    unsigned w = 0, h = 0;
                    std::memcpy(&w, p, 4);
                    std::memcpy(&h, p + 4, 4);
                    if (w > 0 && h > 0 && w < 16384 && h < 16384 && static_cast<size_t>(w) * h * 4 + 8 <= n) {
                        r.w = static_cast<int>(w);
                        r.h = static_cast<int>(h);
                        r.bgra = p + 8;
                    }
                }
            }
        }
#endif
        return r;
    }();
    return img;
}

struct BannerSpan {
    int l = 0, t = 0, r = 0, b = 0;
    bool known = false;
};
BannerSpan g_bannerTitle, g_bannerControl;

// The banner is one picture spread over the parameter's title area and its control area: each
// draw event paints its own share, sized to the whole row (cover fit, centred).
PF_Err drawBanner(PF_InData* in_data, PF_EventExtra* ev) {
    const BannerImage& bm = bannerImage();
    if (!bm.bgra) {
        static bool logged = false;
        if (!logged) { logged = true; logLine("banner: resource LZ_BANNER_IMAGE not found"); }
        return PF_Err_NONE;
    }
    BannerSpan cur;
    cur.l = ev->effect_win.current_frame.left;
    cur.t = ev->effect_win.current_frame.top;
    cur.r = ev->effect_win.current_frame.right;
    cur.b = ev->effect_win.current_frame.bottom;
    cur.known = true;
    if (ev->effect_win.area == PF_EA_PARAM_TITLE) g_bannerTitle = cur;
    else if (ev->effect_win.area == PF_EA_CONTROL) g_bannerControl = cur;
    else return PF_Err_NONE;
    const int fw = cur.r - cur.l, fh = cur.b - cur.t;
    if (fw <= 0 || fh <= 0 || fw > 4096 || fh > 4096) return PF_Err_NONE;

    BannerSpan row = cur;
    if (g_bannerTitle.known && g_bannerControl.known && std::abs(g_bannerTitle.t - g_bannerControl.t) <= 2) {
        row.l = std::min(g_bannerTitle.l, g_bannerControl.l);
        row.r = std::max(g_bannerTitle.r, g_bannerControl.r);
        row.t = std::min(g_bannerTitle.t, g_bannerControl.t);
        row.b = std::max(g_bannerTitle.b, g_bannerControl.b);
    }
    const double rw = row.r - row.l, rh = row.b - row.t;
    const double sc = std::max(rw / bm.w, rh / bm.h);

    std::vector<unsigned char> buf(static_cast<size_t>(fw) * fh * 4);
    auto fetch = [&](double sx, double sy, float* c) {
        sx = std::min(std::max(sx - 0.5, 0.0), bm.w - 1.001);
        sy = std::min(std::max(sy - 0.5, 0.0), bm.h - 1.001);
        const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
        const float fx = static_cast<float>(sx - x0), fy = static_cast<float>(sy - y0);
        const unsigned char* p00 = bm.bgra + (static_cast<size_t>(y0) * bm.w + x0) * 4;
        const unsigned char* p10 = p00 + 4;
        const unsigned char* p01 = p00 + static_cast<size_t>(bm.w) * 4;
        const unsigned char* p11 = p01 + 4;
        for (int k = 0; k < 3; ++k) {
            const float a = p00[k] + (p10[k] - p00[k]) * fx;
            const float b = p01[k] + (p11[k] - p01[k]) * fx;
            c[k] += a + (b - a) * fy;
        }
    };
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            float c[3] = {0, 0, 0};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const double X = cur.l - row.l + x + 0.25 + 0.5 * i, Y = cur.t - row.t + y + 0.25 + 0.5 * j;
                    fetch((X - rw * 0.5) / sc + bm.w * 0.5, (Y - rh * 0.5) / sc + bm.h * 0.5, c);
                }
            unsigned char* q = &buf[(static_cast<size_t>(y) * fw + x) * 4];
            for (int k = 0; k < 3; ++k) q[k] = static_cast<unsigned char>(std::lround(std::min(c[k] * 0.25f, 255.0f)));
            q[3] = 255;
        }

    SPBasicSuite* sp = in_data->pica_basicP;
    const PF_EffectCustomUISuite1* uiS = nullptr;
    const DRAWBOT_DrawbotSuite1* drawS = nullptr;
    const DRAWBOT_SupplierSuite1* supS = nullptr;
    const DRAWBOT_SurfaceSuite1* surfS = nullptr;
    const bool haveSuites =
        sp && sp->AcquireSuite(kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1, reinterpret_cast<const void**>(&uiS)) == kSPNoError && uiS &&
        sp->AcquireSuite(kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent, reinterpret_cast<const void**>(&drawS)) == kSPNoError && drawS &&
        sp->AcquireSuite(kDRAWBOT_SupplierSuite, kDRAWBOT_SupplierSuite_VersionCurrent, reinterpret_cast<const void**>(&supS)) == kSPNoError && supS &&
        sp->AcquireSuite(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_VersionCurrent, reinterpret_cast<const void**>(&surfS)) == kSPNoError && surfS;
    PF_Err err = PF_Err_NONE;
    if (haveSuites) {
        DRAWBOT_DrawRef drawRef = nullptr;
        DRAWBOT_SupplierRef supplierRef = nullptr;
        DRAWBOT_SurfaceRef surfaceRef = nullptr;
        DRAWBOT_ImageRef imageRef = nullptr;
        err = uiS->PF_GetDrawingReference(ev->contextH, &drawRef);
        if (!err) err = drawS->GetSupplier(drawRef, &supplierRef);
        if (!err) err = drawS->GetSurface(drawRef, &surfaceRef);
        if (!err) err = supS->NewImageFromBuffer(supplierRef, fw, fh, fw * 4, kDRAWBOT_PixelLayout_32BGRA_Straight, buf.data(), &imageRef);
        if (!err) {
            DRAWBOT_PointF32 origin;
            origin.x = static_cast<float>(cur.l);
            origin.y = static_cast<float>(cur.t);
            err = surfS->DrawImage(surfaceRef, imageRef, &origin, 1.0f);
        }
        if (imageRef) supS->ReleaseObject((DRAWBOT_ObjectRef)imageRef);
        if (err) logLine("banner: drawing failed, error " + std::to_string(static_cast<int>(err)));
        ev->evt_out_flags |= PF_EO_HANDLED_EVENT;
    } else {
        static bool logged = false;
        if (!logged) { logged = true; logLine("banner: drawing suites not available"); }
    }
    if (sp) {
        if (surfS) sp->ReleaseSuite(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_VersionCurrent);
        if (supS) sp->ReleaseSuite(kDRAWBOT_SupplierSuite, kDRAWBOT_SupplierSuite_VersionCurrent);
        if (drawS) sp->ReleaseSuite(kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent);
        if (uiS) sp->ReleaseSuite(kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1);
    }
    return PF_Err_NONE;
}

PF_Err handleEvent(PF_InData* in_data, PF_EventExtra* ev) {
    {
        // The first events are written to the log so the panel geometry can be inspected.
        static int logged = 0;
        if (ev && ev->contextH && logged < 60 && ev->e_type != PF_Event_IDLE && ev->e_type != PF_Event_ADJUST_CURSOR) {
            ++logged;
            logLine("event type " + std::to_string(static_cast<int>(ev->e_type)) + " window " + std::to_string(static_cast<int>((*ev->contextH)->w_type)) +
                    " index " + std::to_string(static_cast<int>(ev->effect_win.index)) + " area " + std::to_string(static_cast<int>(ev->effect_win.area)) +
                    " frame " + std::to_string(ev->effect_win.current_frame.left) + "," + std::to_string(ev->effect_win.current_frame.top) + "," +
                    std::to_string(ev->effect_win.current_frame.right) + "," + std::to_string(ev->effect_win.current_frame.bottom));
        }
    }
    if (!ev || !ev->contextH || (*ev->contextH)->w_type != PF_Window_EFFECT) return PF_Err_NONE;
    if (ev->e_type != PF_Event_DRAW || ev->effect_win.index != P_BANNER) return PF_Err_NONE;
    return drawBanner(in_data, ev);
}
#endif

inline double ratio(const PF_RationalScale& r) { return r.den ? double(r.num) / double(r.den) : 1.0; }

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    logStep("PreRender begin");
    ParamReader pr(in_data);
    const double maxBlur = pr.num(P_MAX_BLUR);
    const double squeeze = std::max(pr.num(P_SQUEEZE), 1.0);
    const int defMode = static_cast<int>(pr.num(P_DEFOCUS_MODE));
    const bool uniform = defMode != 2 && defMode != 4; // uniform and focus region never exceed the amount
    const double amount = std::fabs(pr.num(P_DEFOCUS_AMOUNT)) * pr.num(P_DEFOCUS_SCALE) / 100.0;
    const double fieldCurv = std::fabs(pr.num(P_FIELD_CURVATURE)) + std::fabs(pr.num(P_FILMBACK_OFFSET));
    const int view = static_cast<int>(pr.num(P_VIEW));
    if (!pr.ok()) return pr.err();

    // Pad the input only by the blur that is actually used: with uniform defocus that is the
    // slider value, with a depth map the Max Blur Radius. Field curvature can add blur at the
    // corners, so fall back to Max Blur when it is on.
    double reach = maxBlur;
    if (uniform && fieldCurv == 0.0 && view != 3) reach = std::min(maxBlur, amount);
    const double dsx = ratio(in_data->downsample_x), dsy = ratio(in_data->downsample_y);
    // The PSF frame reaches ~1.8 blur radii from its centre (cat-eye and coma tails included).
    const A_long padX = static_cast<A_long>(std::ceil(reach * 1.8 * dsx)) + 2;
    const A_long padY = static_cast<A_long>(std::ceil(reach * 1.8 * squeeze * dsy)) + 2;

    PF_RenderRequest req = extra->input->output_request;
    PF_RenderRequest inReq = req;
    inReq.rect.left -= padX; inReq.rect.right += padX;
    inReq.rect.top -= padY; inReq.rect.bottom += padY;
    inReq.preserve_rgb_of_zero_alpha = TRUE;

    PF_CheckoutResult inRes;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, CHECKOUT_INPUT, &inReq,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &inRes));

    // Depth and aperture layers are sampled stretched to fit, so ask for all of them.
    PF_RenderRequest fullReq = req;
    fullReq.rect.left = fullReq.rect.top = -30000;
    fullReq.rect.right = fullReq.rect.bottom = 30000;
    fullReq.preserve_rgb_of_zero_alpha = TRUE;
    PF_CheckoutResult depthRes, apRes;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_DEPTH_LAYER, CHECKOUT_DEPTH, &fullReq,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &depthRes));
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_CUSTOM_APERTURE, CHECKOUT_APERTURE, &fullReq,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &apRes));
    PF_CheckoutResult rainRes;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_RAIN_LAYER, CHECKOUT_RAIN, &fullReq,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &rainRes));
    if (err) return err;

    // Output: what was asked for, within what the input can provide (the layer does not grow).
    PF_LRect out = req.rect;
    out.left = std::max(out.left, inRes.max_result_rect.left);
    out.top = std::max(out.top, inRes.max_result_rect.top);
    out.right = std::min(out.right, inRes.max_result_rect.right);
    out.bottom = std::min(out.bottom, inRes.max_result_rect.bottom);
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;

    extra->output->result_rect = out;
    extra->output->max_result_rect = inRes.max_result_rect;
    extra->output->solid = FALSE;

    PreRenderData* prd = new (std::nothrow) PreRenderData;
    if (!prd) return PF_Err_OUT_OF_MEMORY;
    prd->inRect = inRes.result_rect;
    prd->outRect = out;
    prd->layerW = inRes.ref_width > 0 ? inRes.ref_width : in_data->width;
    prd->layerH = inRes.ref_height > 0 ? inRes.ref_height : in_data->height;
    prd->par = ratio(inRes.par);
    if (prd->par <= 0) prd->par = ratio(in_data->pixel_aspect_ratio);
    extra->output->pre_render_data = prd;
    extra->output->delete_pre_render_data_func = deletePreRenderData;
    logStep("PreRender end");
    return err;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    logStep("SmartRender begin");
    const PreRenderData* prd = static_cast<const PreRenderData*>(extra->input->pre_render_data);
    if (!prd) return PF_Err_BAD_CALLBACK_PARAM;

    PF_EffectWorld *inW = nullptr, *outW = nullptr, *depthW = nullptr, *apW = nullptr, *rainW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err) {
        extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_DEPTH, &depthW);
        extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_APERTURE, &apW);
        extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_RAIN, &rainW);
    }

    if (!err && inW && outW) {
        WorldView in, out, depth, ap, rain;
        in.w = inW; out.w = outW; depth.w = depthW; ap.w = apW; rain.w = rainW;
        ERR(pixelFormat(in_data, inW, in.fmt));
        ERR(pixelFormat(in_data, outW, out.fmt));
        if (depthW) ERR(pixelFormat(in_data, depthW, depth.fmt));
        if (apW) ERR(pixelFormat(in_data, apW, ap.fmt));
        if (rainW) ERR(pixelFormat(in_data, rainW, rain.fmt));

        logStep("SmartRender worlds ready");
        ParamReader pr(in_data);
        RenderSettings rs;
        OpticsSettings& o = rs.optics;
        const double dsx = ratio(in_data->downsample_x), dsy = ratio(in_data->downsample_y);

        o.lensPreset = static_cast<int>(pr.num(P_LENS_PRESET)) - 1;
        o.lens.focalLengthMm = lensPreset(o.lensPreset).focalMm; // every preset keeps its own focal length
        o.lens.fNumber = pr.num(P_FSTOP);
        const int format = static_cast<int>(pr.num(P_FORMAT));
        o.sensorWidthMm = format >= 1 && format < kFormatCount ? kFormatWidths[format - 1] : pr.num(P_SENSOR_WIDTH);

        DefocusSettings& d = rs.defocus;
        const int mode = static_cast<int>(pr.num(P_DEFOCUS_MODE));
        const bool aiDepth = mode == 4;
        d.mode = (mode == 2 || aiDepth) ? DefocusSettings::kDepthMap : (mode == 3 ? DefocusSettings::kRegion : DefocusSettings::kUniform);
        const int aiDetail = static_cast<int>(pr.num(P_AI_DETAIL));
        const bool aiRefine = pr.num(P_AI_REFINE) != 0;
        const bool aiGpu = pr.num(P_AI_GPU) != 0;
        DepthAdjust aiAdj;
        aiAdj.farPoint = static_cast<float>(pr.num(P_AI_FAR) / 100.0);
        aiAdj.nearPoint = static_cast<float>(pr.num(P_AI_NEAR) / 100.0);
        aiAdj.gamma = static_cast<float>(pr.num(P_AI_CONTRAST));
        aiAdj.shift = static_cast<float>(pr.num(P_AI_SHIFT) / 100.0);
        aiAdj.invert = pr.num(P_AI_INVERT) != 0;
        aiAdj.range = static_cast<float>(pr.num(P_AI_RANGE) / 100.0);
        const bool aiFocusPoint = pr.num(P_AI_FOCUS_POINT) != 0;
        const double aiSmoothPx = pr.num(P_AI_SMOOTH);
        d.regionRadiusPx = pr.num(P_REGION_RADIUS);
        d.regionFalloffPx = pr.num(P_REGION_FALLOFF);
        d.regionAspect = pr.num(P_REGION_ASPECT);
        d.regionKeepCenter = pr.num(P_REGION_KEEP) != 0;
        d.amountPx = pr.num(P_DEFOCUS_AMOUNT);
        d.whiteIsNear = pr.num(P_DEPTH_WHITE) < 2;
        d.inverseDepth = pr.num(P_DEPTH_ENCODING) >= 2;
        d.nearMm = pr.num(P_DEPTH_NEAR) * 1000.0;
        d.farMm = pr.num(P_DEPTH_FAR) * 1000.0;
        d.focusMm = pr.num(P_FOCUS_DISTANCE) * 1000.0;
        d.focusFromPoint = aiDepth ? aiFocusPoint : pr.num(P_FOCUS_PICK) != 0;
        double fpx, fpy;
        pr.point(P_FOCUS_POINT, fpx, fpy);
        d.focusPointX = fpx / dsx; // point values arrive at the current downsample
        d.focusPointY = fpy / dsy;
        d.scale = pr.num(P_DEFOCUS_SCALE) / 100.0;
        d.edgeCleanPx = pr.num(P_EDGE_CLEAN);
        o.lens.focusDistanceMm = d.focusMm;

        ApertureShape& a = o.aperture;
        a.shape = static_cast<int>(pr.num(P_APERTURE_SHAPE)) - 1;
        a.blades = static_cast<int>(pr.num(P_BLADES));
        if (a.blades > 0 && a.blades < 3) a.blades = 3;
        a.curvature = pr.num(P_BLADE_CURVATURE) / 100.0;
        a.rotationRad = pr.num(P_BLADE_ROTATION) * kPi / 180.0;
        a.obstruction = pr.num(P_OBSTRUCTION) / 100.0;
        a.obstructionSoftness = pr.num(P_OBSTRUCTION_SOFT) / 100.0;

        o.lens.vignetting = pr.num(P_CATEYE) / 100.0;
        const bool caOn = pr.num(P_CA_ENABLE) != 0;
        o.lens.dispersion = caOn ? pr.num(P_CHROMATIC) / 100.0 : 0.0;
        rs.lateralCaPx = caOn ? pr.num(P_LATERAL_CA) : 0.0;
        o.impression = pr.num(P_IMPRESSION) / 100.0;
        o.impressionPower = pr.num(P_IMPRESSION_POWER);
        o.coverage = pr.num(P_COVERAGE);
        rs.filmbackOffsetMm = pr.num(P_FILMBACK_OFFSET);
        rs.blendBack = pr.num(P_BLEND_BACK);
        a.lobePower = pr.num(P_LOBE_POWER);
        a.lobeAngle = pr.num(P_LOBE_ANGLE) * kPi / 180.0;
        a.lobesFaceCenter = pr.num(P_LOBES_FACE_CENTER) != 0;
        o.coma = pr.num(P_COMA) / 100.0;
        o.astigmatismMm = pr.num(P_ASTIGMATISM);
        rs.fieldCurvatureMm = pr.num(P_FIELD_CURVATURE);
        rs.squeeze = pr.num(P_SQUEEZE);
        a.onion = pr.num(P_ZONAL_RIPPLE) / 100.0;
        a.onionFreq = pr.num(P_RIPPLE_DENSITY);
        a.lobes = pr.num(P_LOBES) / 100.0;
        a.lobeCount = static_cast<int>(pr.num(P_LOBE_COUNT));
        a.texture = pr.num(P_TEXTURE) / 100.0;
        a.textureScale = pr.num(P_TEXTURE_SCALE) / 100.0;
        a.textureSeed = static_cast<int>(pr.num(P_TEXTURE_SEED));
        rs.rain.distanceMm = pr.num(P_RAIN_DISTANCE) * 1000.0;
        rs.rain.strength = pr.num(P_RAIN_ENABLE) != 0 ? pr.num(P_RAIN_STRENGTH) / 100.0 : 0.0;
        rs.rain.refractPx = pr.num(P_RAIN_REFRACTION);
        rs.shimmer.amount = pr.num(P_SHIMMER) / 100.0;
        rs.shimmer.density = pr.num(P_SHIMMER_DENSITY);
        // The seed moves with time so every highlight twinkles from frame to frame.
        const A_long frame = in_data->time_step > 0 ? in_data->current_time / in_data->time_step : 0;
        rs.shimmer.seed = static_cast<int>(pr.num(P_SHIMMER_SEED)) * 7919 + static_cast<int>(frame);

        rs.highlights.threshold = pr.num(P_HI_THRESHOLD) / 100.0;
        rs.highlights.gain = pr.num(P_HI_GAIN);

        o.quality = static_cast<int>(pr.num(P_QUALITY)) - 1;
        o.maxBlurPx = pr.num(P_MAX_BLUR);
        rs.layers = static_cast<int>(pr.num(P_LAYERS));
        const int colorMode = static_cast<int>(pr.num(P_COLOR_MODE));
        double ocx, ocy;
        pr.point(P_OPTICAL_CENTER, ocx, ocy);
        rs.view = static_cast<int>(pr.num(P_VIEW)) - 1;
        rs.view3dYawDeg = pr.num(P_3D_YAW);
        rs.view3dPitchDeg = pr.num(P_3D_PITCH);
        rs.view3dRelief = pr.num(P_3D_RELIEF) / 100.0;
        if (pr.num(P_SHOW_3D) != 0) rs.view = RenderSettings::kDepth3D;
        else if (pr.num(P_SHOW_DEPTH) != 0) rs.view = RenderSettings::kDepthView;
        ERR(pr.err());

        o.pixelAspect = prd->par;
        FrameMapping& fm = rs.frame;
        fm.originX = prd->inRect.left;
        fm.originY = prd->inRect.top;
        fm.downsampleX = dsx;
        fm.downsampleY = dsy;
        fm.layerW = prd->layerW;
        fm.layerH = prd->layerH;
        fm.centerX = ocx / dsx;
        fm.centerY = ocy / dsy;

        const bool decode = colorMode == 3 || (colorMode == 1 && in.fmt != PF_PixelFormat_ARGB128);

        std::vector<float> depthBuf, maskBuf;
        if (!err && d.mode == DefocusSettings::kDepthMap && depthW) {
            int dw, dh;
            worldLuminance(depth, 4096, false, depthBuf, dw, dh);
            for (float& v : depthBuf) v = std::min(std::max(v, 0.0f), 1.0f);
            d.depth = depthBuf.data();
            d.depthW = dw;
            d.depthH = dh;
        }
        std::vector<float> rainBuf;
        if (!err && rainW) {
            int rw, rh;
            worldLuminance(rain, 2048, true, rainBuf, rw, rh);
            rs.rain.map = rainBuf.data();
            rs.rain.w = rw;
            rs.rain.h = rh;
        }
        if (!err && apW) {
            worldLuminance(ap, 128, true, maskBuf, a.maskW, a.maskH);
            a.mask = maskBuf;
        }

        if (!err) {
            Image src, dst;
            const int W = in.width(), H = in.height();
            src.resize(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float* p = src.px(x, y);
                    in.read(x, y, p);
                    if (decode && p[3] > 0) {
                        const float ia = 1.0f / p[3];
                        for (int c = 0; c < 3; ++c) p[c] = srgbDecode(p[c] * ia) * p[3];
                    }
                }

            // AI depth: estimate the depth of the layer itself (the buffer may include padding
            // around it), then use it exactly like a depth layer, white = near, disparity.
            std::vector<float> aiDepthBuf;
            bool aiFailed = false;
            if (aiDepth) {
                const int lx0 = std::max(0, static_cast<int>(-prd->inRect.left));
                const int ly0 = std::max(0, static_cast<int>(-prd->inRect.top));
                const int lx1 = std::min(W, static_cast<int>(std::lround(prd->layerW * dsx - prd->inRect.left)));
                const int ly1 = std::min(H, static_cast<int>(std::lround(prd->layerH * dsy - prd->inRect.top)));
                const int cw = lx1 - lx0, ch = ly1 - ly0;
                std::string aiErr;
                DepthAIConfig cfg;
                cfg.runtimeLib = pluginFolder() + "lensyum_ort.dll";
                cfg.modelPath = pluginFolder() + "lensyum_depth.onnx";
                cfg.useGpu = aiGpu;
                if (cw > 1 && ch > 1 && depthAIInit(cfg, aiErr)) {
                    std::vector<float> rgb(static_cast<size_t>(cw) * ch * 3);
                    for (int y = 0; y < ch; ++y)
                        for (int x = 0; x < cw; ++x) {
                            const float* p = src.px(x + lx0, y + ly0);
                            const float ia = p[3] > 0 ? 1.0f / p[3] : 0.0f;
                            for (int c = 0; c < 3; ++c) rgb[(static_cast<size_t>(y) * cw + x) * 3 + c] = srgbEncode(std::max(p[c] * ia, 0.0f));
                        }
                    const int sides[4] = {392, 518, 770, 1022};
                    aiFailed = !depthAIEstimate(rgb.data(), cw, ch, sides[std::min(std::max(aiDetail, 1), 4) - 1], aiRefine, aiDepthBuf, aiErr);
                } else {
                    aiFailed = true;
                }
                if (aiFailed) {
#ifdef AE_OS_WIN
                    // The reason goes to %TEMP%\lensyum_log.txt so a failure is diagnosable.
                    char tmp[MAX_PATH + 1] = {0};
                    if (GetTempPathA(MAX_PATH, tmp) > 0) {
                        FILE* f = std::fopen((std::string(tmp) + "lensyum_log.txt").c_str(), "w");
                        if (f) {
                            std::fprintf(f, "AI depth failed: %s\nruntime: %s\nmodel: %s\n", aiErr.empty() ? "layer too small" : aiErr.c_str(),
                                         cfg.runtimeLib.c_str(), cfg.modelPath.c_str());
                            std::fclose(f);
                        }
                    }
#endif
                }
                if (!aiFailed) {
                    aiAdj.smoothRadius = static_cast<float>(aiSmoothPx * cw / std::max(prd->layerW, 1.0));
                    if (aiAdj.range > 0.0f) {
                        aiAdj.focusU = static_cast<float>(d.focusPointX / std::max(prd->layerW, 1.0));
                        aiAdj.focusV = static_cast<float>(d.focusPointY / std::max(prd->layerH, 1.0));
                    }
                    adjustDepth(aiDepthBuf, cw, ch, aiAdj);
                    d.depth = aiDepthBuf.data();
                    d.depthW = cw;
                    d.depthH = ch;
                    d.whiteIsNear = true;
                    d.inverseDepth = true;
                } else {
                    d.mode = DefocusSettings::kUniform;
                    d.amountPx = 0.0;
                }
            }

            logStep("SmartRender before renderDefocus");
            renderDefocus(src, dst, rs);
            logStep("SmartRender after renderDefocus");

            // Missing model or runtime: show the frame flat red so it cannot be mistaken for an effect.
            if (aiFailed)
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        float* p = dst.px(x, y);
                        p[0] = p[3] > 0 ? p[3] : 0.0f; p[1] *= 0.05f; p[2] *= 0.05f;
                    }

            const A_long ox = prd->outRect.left - prd->inRect.left;
            const A_long oy = prd->outRect.top - prd->inRect.top;
            const bool encode = decode;
            for (int y = 0; y < out.height(); ++y) {
                const int sy = static_cast<int>(y + oy);
                for (int x = 0; x < out.width(); ++x) {
                    const int sx = static_cast<int>(x + ox);
                    float p[4] = {0, 0, 0, 0};
                    if (sx >= 0 && sy >= 0 && sx < W && sy < H) {
                        const float* q = dst.px(sx, sy);
                        p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; p[3] = q[3];
                        if (encode && p[3] > 0) {
                            const float ia = 1.0f / p[3];
                            for (int c = 0; c < 3; ++c) p[c] = srgbEncode(std::max(p[c] * ia, 0.0f)) * p[3];
                        }
                    }
                    out.write(x, y, p);
                }
            }
        }
    }

    logStep("SmartRender copied output");
    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_INPUT));
    if (depthW) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_DEPTH));
    if (apW) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_APERTURE));
    if (rainW) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_RAIN));
    return err ? err : err2;
}

PF_Err About(PF_InData* in_data, PF_OutData* out_data) {
    PF_SPRINTF(out_data->return_msg, "%s %d.%d (build %s %s)\rPhysically based lens defocus: real lens prescriptions, ray-traced bokeh.",
               LENSYUM_NAME, LENSYUM_MAJOR, LENSYUM_MINOR, __DATE__, __TIME__);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    logStep("GlobalSetup");
    out_data->my_version = PF_VERSION(LENSYUM_MAJOR, LENSYUM_MINOR, LENSYUM_BUG, PF_Stage_DEVELOP, LENSYUM_BUILD);
    // Must match AE_Effect_Global_OutFlags / _2 in LensyumPiPL.r (and the 'global out flags' values in LensyumPiPL.rc).
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;
#ifdef LENSYUM_BANNER
    out_data->out_flags |= PF_OutFlag_CUSTOM_UI;
#endif
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

} // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, LENSYUM_NAME, LENSYUM_MATCH_NAME, LENSYUM_CATEGORY,
                                     AE_RESERVED_INFO, "EffectMain", "https://github.com/ramirstudio/lensyum");
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    {
        // Each command number is written to the log the first time it arrives.
        static unsigned long long seen = 0;
        if (cmd >= 0 && cmd < 64 && !(seen & (1ULL << cmd))) {
            seen |= 1ULL << cmd;
            logLine(std::string("build ") + __DATE__ + " " + __TIME__ + ": first command " + std::to_string(static_cast<int>(cmd)));
        }
    }
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT: err = About(in_data, out_data); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data); break;
        case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra)); break;
        case PF_Cmd_SMART_RENDER: err = SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra)); break;
#ifdef LENSYUM_BANNER
        case PF_Cmd_EVENT: err = handleEvent(in_data, static_cast<PF_EventExtra*>(extra)); break;
#endif
        default: break;
        }
    } catch (const std::bad_alloc&) {
        logLine("exception: out of memory");
        err = PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        logLine("exception: unknown");
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
