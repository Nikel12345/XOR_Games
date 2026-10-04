#include "PCH.h"
#include "GameComponents.h"
#include <cfloat>

void RegisterGameComponents()
{
    using enum FieldKind;
    auto& reg = ComponentSpecRegistry::Get();

    reg.Register({ .name = "Mass", .sig_type = typeid(MassComponent),
        .add_default = AddDefaultAoS<MassComponent>,
        .fields = { FieldSpec::Num("mass", F32, AOS_NUM(MassComponent, mass), 0, FLT_MAX) } });

    reg.Register({ .name = "Gravity", .sig_type = typeid(GravityComponent),
        .add_default = AddDefaultAoS<GravityComponent>,
        .fields = { FieldSpec::Num("gm", F32, AOS_NUM(GravityComponent, gm), 0, FLT_MAX, 1.0f),
                    FieldSpec::Num("id", U32, AOS_NUM(GravityComponent, id), 0, 0, 1),
                    FieldSpec::Num("core_radius", F32, AOS_NUM(GravityComponent, core_radius), 0, FLT_MAX, 1.0f) } });

    reg.Register({ .name = "Jet", .sig_type = typeid(JetComponent),
        .add_default = AddDefaultAoS<JetComponent>,
        .fields = { FieldSpec::Num("center", U32, AOS_NUM(JetComponent, center), 0, 0, 1) } });

    reg.Register({ .name = "GravityWorld", .sig_type = typeid(GravityWorldComponent),
        .add_default = AddDefaultAoS<GravityWorldComponent>,
        .fields = { FieldSpec::Num("sim_dt", F32, AOS_NUM(GravityWorldComponent, sim_dt), 0, FLT_MAX, 0.001f),
                    FieldSpec::Num("jet_return_distance", F32, AOS_NUM(GravityWorldComponent, jet_return_distance), 0, FLT_MAX, 1.0f) } });

    reg.Register({ .name = "GravitationalLens", .sig_type = typeid(GravitationalLensComponent),
        .add_default = AddDefaultAoS<GravitationalLensComponent>,
        .fields = { FieldSpec::Num("schwarzschild_radius", F32, AOS_NUM(GravitationalLensComponent, schwarzschild_radius), 0, FLT_MAX, 0.1f),
                    FieldSpec::Num("inner_radius", F32, AOS_NUM(GravitationalLensComponent, inner_radius), 0, FLT_MAX, 0.1f) } });
}
