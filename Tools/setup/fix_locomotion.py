# Retunes the template mannequin's locomotion blend space for mountaineering speeds (UnrealEditor-Cmd -run=pythonscript).
#
# The template places its walk cycle at 300 cm/s. High-altitude trekking runs at 50-150 cm/s, which left the
# body blended two-thirds toward idle: short, shuffling steps that read as a limp. Adding the same walk clips
# at 50 and 150 cm/s with proportionally lower play rates keeps a full stride; blend-space samples play in
# normalised-time lockstep, so blending one clip at different rates just slows the cycle smoothly.
import unreal

BS_PATH = "/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run"
TEMPLATE_WALK_SPEED = 300.0
SLOW_SPEEDS = (50.0, 150.0)
# Below ~0.5 m/s the template stride slides a little; a slightly faster cycle hides it.
MIN_RATE = 0.3

bs = unreal.load_asset(BS_PATH)
samples = list(bs.get_editor_property("sample_data"))
existing = {(round(s.get_editor_property("sample_value").x), round(s.get_editor_property("sample_value").y)) for s in samples}
walks = [s for s in samples if round(s.get_editor_property("sample_value").y) == round(TEMPLATE_WALK_SPEED)]

params = list(bs.get_editor_property("blend_parameters"))
speed_axis = params[1]
speed_axis.set_editor_property("grid_num", 12)  # 50 cm/s grid so the new rows sit on grid points
params[1] = speed_axis
bs.set_editor_property("blend_parameters", params)

added = 0
for speed in SLOW_SPEEDS:
    for walk in walks:
        direction = walk.get_editor_property("sample_value").x
        if (round(direction), round(speed)) in existing:
            continue
        sample = unreal.BlendSample()
        sample.set_editor_property("animation", walk.get_editor_property("animation"))
        sample.set_editor_property("sample_value", unreal.Vector(direction, speed, 0.0))
        sample.set_editor_property("rate_scale", max(MIN_RATE, speed / TEMPLATE_WALK_SPEED))
        samples.append(sample)
        added += 1

bs.set_editor_property("sample_data", samples)
unreal.EditorAssetLibrary.save_loaded_asset(bs, only_if_is_dirty=False)

check = unreal.load_asset(BS_PATH).get_editor_property("sample_data")
slow = [s for s in check if round(s.get_editor_property("sample_value").y) in (50, 150)]
assert len(slow) >= len(walks) * len(SLOW_SPEEDS), "slow walk samples were not saved"
print(f"SUMMIT_LOCOMOTION added {added} slow-walk samples; total {len(check)}")
