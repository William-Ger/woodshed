"""
Renders Woodshed's UI art in Blender (headless):

    /Applications/Blender.app/Contents/MacOS/Blender -b -P art/build_assets.py -- <asset> [<asset> ...]

Assets: enclosure, knob, footswitch, led, board, floor, amp, cab, all.
Everything is a top-down (or front-on) orthographic render with a transparent
background, written to art/renders/. Each asset also writes a small JSON file
describing where its important regions sit in the image, so the app can line
its controls up with the pixels.

Pedal enclosures render in three layers so the app can paint any colour:
  enclosure_shade.png  white paint, diffuse only (multiply by the pedal colour)
  enclosure_spec.png   the clear-coat highlights only (added on top)
  enclosure_hw.png     screws and jacks in their real colours (drawn over)
"""

import bpy, bmesh, json, math, os, sys
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "renders")
os.makedirs(OUT, exist_ok=True)


# ---------------------------------------------------------------------------
# Scene helpers

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    s = bpy.context.scene
    s.render.engine = "CYCLES"
    s.cycles.samples = 96
    s.cycles.use_denoising = True
    s.cycles.device = "CPU"   # Metal kernel compilation hangs headless on this machine
    s.render.threads_mode = "AUTO"
    s.render.film_transparent = True
    s.render.image_settings.file_format = "PNG"
    s.render.image_settings.color_mode = "RGBA"
    s.render.image_settings.color_depth = "8"
    s.view_settings.view_transform = "Standard"
    s.view_settings.look = "None"
    world = bpy.data.worlds.new("World")
    s.world = world
    world.use_nodes = True
    # A soft studio: brighter overhead than at the horizon, so chrome has something to reflect.
    nt = world.node_tree
    bg = nt.nodes["Background"]
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0.02, 0.02, 0.022, 1)
    ramp.color_ramp.elements[1].position = 0.85
    ramp.color_ramp.elements[1].color = (0.55, 0.55, 0.58, 1)
    nt.links.new(coord.outputs["Generated"], sep.inputs[0])
    nt.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], bg.inputs["Color"])
    bg.inputs["Strength"].default_value = 0.7
    return s


def camera_top(scale, width_px, height_px, z=50.0, front=False):
    s = bpy.context.scene
    cam_data = bpy.data.cameras.new("Cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = scale
    cam = bpy.data.objects.new("Cam", cam_data)
    s.collection.objects.link(cam)
    if front:
        cam.location = (0, -z, 0)
        cam.rotation_euler = (math.radians(90), 0, 0)
    else:
        cam.location = (0, 0, z)
        cam.rotation_euler = (0, 0, 0)
    cam_data.clip_end = z * 3
    s.camera = cam
    s.render.resolution_x = width_px
    s.render.resolution_y = height_px
    s.render.resolution_percentage = 100
    return cam


def area_light(name, loc, energy, size, rot=(0, 0, 0), colour=(1, 1, 1)):
    d = bpy.data.lights.new(name, "AREA")
    d.energy = energy
    d.size = size
    d.color = colour
    o = bpy.data.objects.new(name, d)
    o.location = loc
    o.rotation_euler = rot
    bpy.context.scene.collection.objects.link(o)
    return o


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def studio_lights(front=False):
    """Key light from the top-left (matches the app's shading), soft fill, faint rim."""
    if front:
        key = area_light("Key", (-14, -20, 16), 2600, 14)
        fill = area_light("Fill", (16, -18, 4), 700, 18)
        look_at(key, (0, 0, 0)); look_at(fill, (0, 0, 0))
    else:
        key = area_light("Key", (-12, 14, 22), 3200, 12)
        fill = area_light("Fill", (14, -10, 18), 700, 20)
        rim = area_light("Rim", (0, 20, 6), 300, 10)
        for l in (key, fill, rim):
            look_at(l, (0, 0, 0))


def shadow_catcher(size=60, z=0.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0, 0, z))
    p = bpy.context.active_object
    p.name = "ShadowCatcher"
    p.is_shadow_catcher = True
    return p


def material(name, base=(0.8, 0.8, 0.8), rough=0.4, metal=0.0, spec=0.5, coat=0.0, emission=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*base, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    b.inputs["Specular IOR Level"].default_value = spec
    b.inputs["Coat Weight"].default_value = coat
    b.inputs["Coat Roughness"].default_value = 0.12
    if emission:
        b.inputs["Emission Color"].default_value = (*emission, 1)
        b.inputs["Emission Strength"].default_value = 2.0
    return m


def holdout(name="Holdout"):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    h = nt.nodes.new("ShaderNodeHoldout")
    nt.links.new(h.outputs[0], out.inputs[0])
    return m


def assign(obj, mat):
    obj.data.materials.clear()
    obj.data.materials.append(mat)


def bevel(obj, width, segments=5, angle=40):
    mod = obj.modifiers.new("Bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    mod.angle_limit = math.radians(angle)
    mod.harden_normals = False
    return mod


def smooth(obj):
    for p in obj.data.polygons:
        p.use_smooth = True


def render(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("wrote", path)


def write_json(name, data):
    with open(os.path.join(OUT, name + ".json"), "w") as f:
        json.dump(data, f, indent=2)


def rounded_box(name, sx, sy, sz, corner, top_taper=0.97, z0=0.0):
    """A die-cast style box: rounded vertical corners, slightly drafted sides, bevelled top edge."""
    bm = bmesh.new()
    segs = 8
    pts = []
    for cx, cy, a0 in ((sx / 2 - corner, sy / 2 - corner, 0), (-sx / 2 + corner, sy / 2 - corner, 90),
                       (-sx / 2 + corner, -sy / 2 + corner, 180), (sx / 2 - corner, -sy / 2 + corner, 270)):
        for i in range(segs + 1):
            a = math.radians(a0 + 90 * i / segs)
            pts.append((cx + corner * math.cos(a), cy + corner * math.sin(a)))
    bottom = [bm.verts.new((x, y, z0)) for x, y in pts]
    top = [bm.verts.new((x * top_taper, y * top_taper, z0 + sz)) for x, y in pts]
    n = len(pts)
    for i in range(n):
        bm.faces.new((bottom[i], bottom[(i + 1) % n], top[(i + 1) % n], top[i]))
    bm.faces.new(list(reversed(bottom)))
    bm.faces.new(top)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    smooth(o)
    return o


def cylinder(name, r, depth, loc, verts=48, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth, location=loc, rotation=rot)
    o = bpy.context.active_object
    o.name = name
    return o


def image_rect(obj_min, obj_max, cam_scale, w, h):
    """Normalised image rect (x, y, w, h; y down) of a world-space XY box under a top camera."""
    aspect = w / h
    sx = cam_scale if aspect >= 1 else cam_scale * aspect
    sy = cam_scale / aspect if aspect >= 1 else cam_scale
    x0 = (obj_min[0] + sx / 2) / sx
    x1 = (obj_max[0] + sx / 2) / sx
    y0 = 1 - (obj_max[1] + sy / 2) / sy
    y1 = 1 - (obj_min[1] + sy / 2) / sy
    return [round(x0, 5), round(y0, 5), round(x1 - x0, 5), round(y1 - y0, 5)]


# ---------------------------------------------------------------------------
# Pedal enclosure

ENC_W, ENC_H, ENC_D = 6.8, 9.6, 2.9


def build_enclosure():
    body = rounded_box("Enclosure", ENC_W, ENC_H, ENC_D, corner=0.55, top_taper=0.975)
    bevel(body, 0.32, segments=6, angle=30)

    hardware = []
    # Screws in the corners of the lid.
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * (ENC_W / 2 - 0.62), sy * (ENC_H / 2 - 0.62)
            head = cylinder("Screw", 0.24, 0.12, (x, y, ENC_D + 0.02), verts=32)
            bevel(head, 0.06, 3, 30)
            slot = cylinder("Slot", 0.03, 0.4, (x, y, ENC_D + 0.1), verts=4)
            slot.scale = (1, 8, 1)
            slot.rotation_euler = (0, 0, math.radians(35 + 20 * sx * sy))
            boolean = head.modifiers.new("Slot", "BOOLEAN")
            boolean.object = slot
            boolean.operation = "DIFFERENCE"
            slot.hide_render = True
            smooth(head)
            hardware.append(head)

    # Jacks: hex nut and sleeve on both sides, 30% down from the top.
    jack_y = ENC_H / 2 - ENC_H * 0.3
    for side in (-1, 1):
        x = side * (ENC_W / 2)
        nut = cylinder("JackNut", 0.42, 0.22, (x + side * 0.08, jack_y, ENC_D * 0.5), verts=6, rot=(0, math.radians(90), 0))
        bevel(nut, 0.04, 2, 30)
        sleeve = cylinder("JackSleeve", 0.3, 0.38, (x + side * 0.25, jack_y, ENC_D * 0.5), verts=32, rot=(0, math.radians(90), 0))
        hardware += [nut, sleeve]

    return body, hardware


def render_enclosure():
    reset()
    studio_lights()
    shadow_catcher()
    body, hardware = build_enclosure()

    scale = 11.2
    w, h = 640, 900
    camera_top(scale, w, h)

    white = material("PaintWhite", base=(0.82, 0.82, 0.82), rough=0.5, spec=0.0)
    gloss = material("PaintGloss", base=(0.0, 0.0, 0.0), rough=0.22, spec=0.6, coat=0.6)
    steel = material("Steel", base=(0.78, 0.78, 0.8), rough=0.28, metal=1.0)
    nickel = material("Nickel", base=(0.62, 0.62, 0.64), rough=0.18, metal=1.0)
    hold = holdout()

    for o in hardware:
        assign(o, nickel if "Jack" in o.name else steel)

    # Layer 1: diffuse paint; hardware held out so its pixels stay clear.
    assign(body, white)
    for o in hardware:
        assign(o, hold)
    render(os.path.join(OUT, "enclosure_shade.png"))

    # Layer 2: highlights only (black glossy paint), no shadow catcher.
    bpy.data.objects["ShadowCatcher"].hide_render = True
    assign(body, gloss)
    render(os.path.join(OUT, "enclosure_spec.png"))

    # Layer 3: hardware in real colours, the body held out.
    bpy.data.objects["ShadowCatcher"].hide_render = False
    assign(body, hold)
    for o in hardware:
        assign(o, nickel if "Jack" in o.name else steel)
    render(os.path.join(OUT, "enclosure_hw.png"))

    write_json("enclosure", {
        "size": [w, h],
        "body": image_rect((-ENC_W / 2, -ENC_H / 2), (ENC_W / 2, ENC_H / 2), scale, w, h),
        "jackY": round(0.3, 4),
    })


# ---------------------------------------------------------------------------
# Knob, footswitch, LED

def fluted_cylinder(name, r_outer, r_inner, depth, flutes, z0):
    bm = bmesh.new()
    n = flutes * 4
    ring_b, ring_t = [], []
    for i in range(n):
        a = 2 * math.pi * i / n
        r = r_outer if (i % 4) in (0, 1) else r_inner
        ring_b.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z0)))
        ring_t.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z0 + depth)))
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((ring_b[i], ring_b[j], ring_t[j], ring_t[i]))
    bm.faces.new(list(reversed(ring_b)))
    bm.faces.new(ring_t)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    return o


def render_knob():
    reset()
    studio_lights()
    shadow_catcher()
    plastic = material("Plastic", base=(0.025, 0.025, 0.028), rough=0.32, spec=0.55)
    alu = material("Aluminium", base=(0.55, 0.55, 0.56), rough=0.32, metal=1.0)

    skirt = cylinder("Skirt", 1.0, 0.22, (0, 0, 0.11), verts=96)
    bevel(skirt, 0.06, 3, 30)
    body = fluted_cylinder("Body", 0.76, 0.735, 0.9, 40, 0.2)
    bevel(body, 0.08, 4, 60)
    cap = cylinder("Cap", 0.42, 0.05, (0, 0, 1.12), verts=96)
    bevel(cap, 0.03, 3, 30)
    for o in (skirt, body, cap):
        smooth(o)
    assign(skirt, plastic); assign(body, plastic); assign(cap, alu)

    scale, px = 2.7, 256
    camera_top(scale, px, px)
    render(os.path.join(OUT, "knob.png"))
    write_json("knob", {"skirt": round(1.0 / scale, 4), "body": round(0.76 / scale, 4), "cap": round(0.42 / scale, 4)})


def render_footswitch():
    reset()
    studio_lights()
    shadow_catcher()
    chrome = material("Chrome", base=(0.9, 0.9, 0.92), rough=0.08, metal=1.0)
    steel = material("Steel", base=(0.7, 0.7, 0.72), rough=0.3, metal=1.0)

    washer = cylinder("Washer", 0.95, 0.06, (0, 0, 0.03), verts=64)
    nut = cylinder("Nut", 0.82, 0.32, (0, 0, 0.22), verts=6)
    bevel(nut, 0.05, 2, 30)
    bushing = cylinder("Bushing", 0.56, 0.3, (0, 0, 0.5), verts=48)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.5, location=(0, 0, 0.62), segments=48, ring_count=24)
    dome = bpy.context.active_object
    dome.scale = (1, 1, 0.75)
    for o in (washer, bushing, dome):
        smooth(o)
    assign(washer, steel); assign(nut, steel); assign(bushing, chrome); assign(dome, chrome)

    scale, px = 2.3, 256
    camera_top(scale, px, px)
    render(os.path.join(OUT, "footswitch.png"))
    write_json("footswitch", {"radius": round(0.95 / scale, 4)})


def render_led():
    for state in ("off", "on"):
        reset()
        studio_lights()
        shadow_catcher()
        chrome = material("Chrome", base=(0.88, 0.88, 0.9), rough=0.12, metal=1.0)
        lens = material("Lens", base=(0.25, 0.01, 0.005), rough=0.08, spec=0.8,
                        emission=(1.0, 0.03, 0.01) if state == "on" else None)
        if state == "on":
            lens.node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 6.0
        ring = cylinder("Bezel", 0.34, 0.16, (0, 0, 0.08), verts=48)
        bevel(ring, 0.05, 3, 30)
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.22, location=(0, 0, 0.14), segments=32, ring_count=16)
        dome = bpy.context.active_object
        dome.scale = (1, 1, 0.7)
        smooth(ring); smooth(dome)
        assign(ring, chrome); assign(dome, lens)
        if state == "on":
            bpy.context.scene.view_settings.look = "None"
        camera_top(1.0, 128, 128)
        render(os.path.join(OUT, f"led_{state}.png"))
    write_json("led", {"bezel": round(0.34 / 1.0, 4)})


# ---------------------------------------------------------------------------
# Pedalboard and floor

BOARD_W, BOARD_H = 32.0, 19.6
RAILS, RAIL_H, RAIL_GAP = 6, 2.35, 0.62
END_W = 1.3


def velcro_material():
    m = material("Velcro", base=(0.018, 0.018, 0.02), rough=0.92, spec=0.2)
    nt = m.node_tree
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 220.0
    noise.inputs["Detail"].default_value = 8.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.6
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], nt.nodes["Principled BSDF"].inputs["Normal"])
    return m


def brushed_material(name, base, rough=0.35):
    m = material(name, base=base, rough=rough, metal=1.0)
    nt = m.node_tree
    coord = nt.nodes.new("ShaderNodeTexCoord")
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (0.02, 6.0, 1.0)   # streaks run along X
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 60.0
    noise.inputs["Detail"].default_value = 10.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.08
    nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
    nt.links.new(mapping.outputs["Vector"], noise.inputs["Vector"])
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], nt.nodes["Principled BSDF"].inputs["Normal"])
    return m


def render_board():
    reset()
    studio_lights()
    shadow_catcher(size=120)

    anodised = brushed_material("Anodised", (0.09, 0.09, 0.1), rough=0.4)
    rail_metal = brushed_material("RailMetal", (0.2, 0.2, 0.215), rough=0.33)
    base_mat = material("Base", base=(0.012, 0.012, 0.014), rough=0.8)
    velcro = velcro_material()
    steel = material("Steel", base=(0.7, 0.7, 0.72), rough=0.3, metal=1.0)

    # Base plate seen through the gaps between rails.
    base = rounded_box("BasePlate", BOARD_W - 0.4, BOARD_H - 0.4, 0.3, 0.6, top_taper=1.0)
    assign(base, base_mat)

    # End caps (left and right) are the tallest parts of the frame.
    for side in (-1, 1):
        cap = rounded_box("EndCap", END_W, BOARD_H, 2.0, 0.45, top_taper=0.96)
        cap.location.x = side * (BOARD_W / 2 - END_W / 2)
        bevel(cap, 0.22, 5, 30)
        assign(cap, anodised)
        for y in (-BOARD_H / 2 + 0.9, BOARD_H / 2 - 0.9):
            sc = cylinder("Screw", 0.2, 0.1, (cap.location.x, y, 2.02), verts=32)
            assign(sc, steel)

    # Rails, each with a velcro strip down the middle.
    inner = BOARD_W - 2 * END_W
    total = RAILS * RAIL_H + (RAILS - 1) * RAIL_GAP
    top_y = total / 2
    rail_rects = []
    for i in range(RAILS):
        y = top_y - RAIL_H / 2 - i * (RAIL_H + RAIL_GAP)
        rail = rounded_box("Rail", inner + 0.2, RAIL_H, 1.35, 0.12, top_taper=0.985)
        rail.location.y = y
        bevel(rail, 0.14, 4, 30)
        assign(rail, rail_metal)
        strip = rounded_box("Velcro", inner - 0.6, RAIL_H * 0.62, 0.05, 0.06, top_taper=1.0, z0=1.35)
        strip.location.y = y
        assign(strip, velcro)
        rail_rects.append((y - RAIL_H / 2, y + RAIL_H / 2))

    scale, w, h = BOARD_W + 1.6, 2400, 1470
    camera_top(scale, w, h)
    render(os.path.join(OUT, "board.png"))

    # Two pedal rows, each spanning three rails.
    rows = []
    for r in range(2):
        y_top = rail_rects[r * 3][1]
        y_bottom = rail_rects[r * 3 + 2][0]
        rows.append(image_rect((-inner / 2, y_bottom), (inner / 2, y_top), scale, w, h))

    write_json("board", {
        "size": [w, h],
        "frame": image_rect((-BOARD_W / 2, -BOARD_H / 2), (BOARD_W / 2, BOARD_H / 2), scale, w, h),
        "rows": rows,
    })


def render_floor():
    reset()
    s = bpy.context.scene
    s.cycles.samples = 64
    area_light("Key", (-30, 25, 40), 9000, 40)
    look_at(bpy.data.objects["Key"], (0, 0, 0))

    m = material("Planks", base=(0.1, 0.06, 0.035), rough=0.55, spec=0.35)
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    coord = nt.nodes.new("ShaderNodeTexCoord")
    bricks = nt.nodes.new("ShaderNodeTexBrick")
    bricks.inputs["Scale"].default_value = 1.0
    bricks.inputs["Brick Width"].default_value = 14.0
    bricks.inputs["Row Height"].default_value = 2.2
    bricks.inputs["Mortar Size"].default_value = 0.025
    bricks.inputs["Color1"].default_value = (0.11, 0.065, 0.038, 1)
    bricks.inputs["Color2"].default_value = (0.075, 0.045, 0.028, 1)
    bricks.inputs["Mortar"].default_value = (0.01, 0.007, 0.005, 1)
    grain_map = nt.nodes.new("ShaderNodeMapping")
    grain_map.inputs["Scale"].default_value = (0.08, 3.0, 1.0)
    grain = nt.nodes.new("ShaderNodeTexWave")
    grain.inputs["Scale"].default_value = 3.0
    grain.inputs["Distortion"].default_value = 6.0
    grain.inputs["Detail"].default_value = 6.0
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 0.45
    nt.links.new(coord.outputs["Object"], bricks.inputs["Vector"])
    nt.links.new(coord.outputs["Object"], grain_map.inputs["Vector"])
    nt.links.new(grain_map.outputs["Vector"], grain.inputs["Vector"])
    nt.links.new(bricks.outputs["Color"], mix.inputs["A"])
    nt.links.new(grain.outputs["Color"], mix.inputs["B"])
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.25
    nt.links.new(bricks.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])

    bpy.ops.mesh.primitive_plane_add(size=80)
    floor = bpy.context.active_object
    assign(floor, m)
    s.render.film_transparent = False
    camera_top(60, 2048, 1280)
    render(os.path.join(OUT, "floor.jpg") if False else os.path.join(OUT, "floor.png"))


# ---------------------------------------------------------------------------
# Amp head and cab (front views; X across, Z up, camera looks along +Y)

def tolex_material():
    m = material("Tolex", base=(0.03, 0.03, 0.032), rough=0.55, spec=0.4)
    nt = m.node_tree
    noise = nt.nodes.new("ShaderNodeTexVoronoi")
    noise.inputs["Scale"].default_value = 90.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.35
    nt.links.new(noise.outputs["Distance"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], nt.nodes["Principled BSDF"].inputs["Normal"])
    return m


def grille_material():
    m = material("Grille", base=(0.11, 0.095, 0.075), rough=0.85, spec=0.15)
    nt = m.node_tree
    coord = nt.nodes.new("ShaderNodeTexCoord")
    w1 = nt.nodes.new("ShaderNodeTexWave")
    w1.inputs["Scale"].default_value = 40.0
    w1.bands_direction = "DIAGONAL"
    w2 = nt.nodes.new("ShaderNodeTexWave")
    w2.inputs["Scale"].default_value = 40.0
    w2.bands_direction = "DIAGONAL"
    map2 = nt.nodes.new("ShaderNodeMapping")
    map2.inputs["Rotation"].default_value = (0, math.radians(90), 0)
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    nt.links.new(coord.outputs["Object"], w1.inputs["Vector"])
    nt.links.new(coord.outputs["Object"], map2.inputs["Vector"])
    nt.links.new(map2.outputs["Vector"], w2.inputs["Vector"])
    nt.links.new(w1.outputs["Fac"], mul.inputs[0])
    nt.links.new(w2.outputs["Fac"], mul.inputs[1])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.05, 0.043, 0.035, 1)
    ramp.color_ramp.elements[1].color = (0.2, 0.17, 0.12, 1)
    nt.links.new(mul.outputs[0], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], nt.nodes["Principled BSDF"].inputs["Base Color"])
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.5
    nt.links.new(mul.outputs[0], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], nt.nodes["Principled BSDF"].inputs["Normal"])
    return m


def front_box(name, w, h, d, corner):
    """A rounded box whose front face points at -Y."""
    o = rounded_box(name, w, h, d, corner, top_taper=1.0)
    o.rotation_euler = (math.radians(90), 0, 0)   # local Z (depth) now points at -Y
    o.location = (0, 0, -h / 2) if False else (0, 0, 0)
    return o


def panel_box(name, x, z, w, h, depth, y_front):
    o = rounded_box(name, w, h, depth, 0.15, top_taper=1.0)
    o.rotation_euler = (math.radians(90), 0, 0)
    o.location = (x, y_front, z)
    bevel(o, 0.06, 3, 30)
    return o


def corner_protectors(w, h, y_front):
    chrome = material("CornerChrome", base=(0.85, 0.85, 0.87), rough=0.15, metal=1.0)
    objs = []
    for sx in (-1, 1):
        for sz in (-1, 1):
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.62, location=(sx * (w / 2 - 0.25), y_front - 0.05, sz * (h / 2 - 0.25)),
                                                 segments=32, ring_count=16)
            c = bpy.context.active_object
            c.scale = (1, 0.45, 1)
            smooth(c)
            assign(c, chrome)
            objs.append(c)
    return objs


def piping(w, h, y_front, inset):
    m = material("Piping", base=(0.75, 0.68, 0.5), rough=0.35, spec=0.5)
    bpy.ops.curve.primitive_bezier_curve_add()
    curve = bpy.context.active_object
    curve.data.splines.clear()
    sp = curve.data.splines.new("POLY")
    pts = [(-w / 2 + inset, h / 2 - inset), (w / 2 - inset, h / 2 - inset), (w / 2 - inset, -h / 2 + inset), (-w / 2 + inset, -h / 2 + inset)]
    sp.points.add(len(pts) - 1)
    for p, (x, z) in zip(sp.points, pts):
        p.co = (x, y_front - 0.02, z, 1)
    sp.use_cyclic_u = True
    curve.data.dimensions = "3D"
    curve.data.bevel_depth = 0.07
    curve.data.bevel_resolution = 4
    curve.data.materials.append(m)
    return curve


def front_rect(xmin, zmin, xmax, zmax, scale, w, h):
    return image_rect((xmin, zmin), (xmax, zmax), scale, w, h)


def render_rig(kind):
    reset()
    studio_lights(front=True)
    tolex, grille = tolex_material(), grille_material()
    white = material("PanelWhite", base=(0.82, 0.82, 0.82), rough=0.45, spec=0.0)
    gloss = brushed_material("PanelGloss", (0.0, 0.0, 0.0), rough=0.25)
    hold = holdout()

    if kind == "amp":
        W, H, D = 12.0, 6.6, 4.0
        panel = (-W / 2 + 0.75, H / 2 - 0.75 - 3.6, W / 2 - 0.75, H / 2 - 0.75)   # xmin, zmin, xmax, zmax
        grille_r = (-W / 2 + 0.75, -H / 2 + 0.75, W / 2 - 0.75, panel[1] - 0.25)
        px, py = 1040, 800
    else:
        W, H, D = 12.0, 11.4, 5.0
        panel = (-W / 2 + 0.75, H / 2 - 0.75 - 4.2, W / 2 - 0.75, H / 2 - 0.75)
        grille_r = (-W / 2 + 0.75, -H / 2 + 0.75, W / 2 - 0.75, panel[1] - 0.3)
        px, py = 1000, 980

    box = rounded_box("Cabinet", W, D, H, 0.5, top_taper=1.0)     # X wide, Y deep, Z tall
    box.location = (0, D / 2, -H / 2)
    bevel(box, 0.35, 6, 30)
    assign(box, tolex)

    front_y = 0.0
    pnl = rounded_box("Panel", panel[2] - panel[0], 0.2, panel[3] - panel[1], 0.12, top_taper=1.0)
    pnl.location = ((panel[0] + panel[2]) / 2, front_y - 0.12, panel[1])
    bevel(pnl, 0.05, 3, 30)

    grl = rounded_box("GrilleCloth", grille_r[2] - grille_r[0], 0.2, grille_r[3] - grille_r[1], 0.15, top_taper=1.0)
    grl.location = ((grille_r[0] + grille_r[2]) / 2, front_y - 0.05, grille_r[1])
    assign(grl, grille)

    pipe = piping(W, H, front_y - 0.02, 0.45)
    corners = corner_protectors(W, H, front_y)

    if kind == "amp":
        handle = rounded_box("Handle", 3.2, 0.9, 0.45, 0.2, top_taper=1.0)
        handle.location = (0, D / 2, H / 2 - 0.05)
        assign(handle, tolex)
    else:
        for sx in (-1, 1):
            foot = rounded_box("Foot", 1.4, 1.2, 0.45, 0.2, top_taper=1.0)
            foot.location = (sx * (W / 2 - 1.4), D / 2, -H / 2 - 0.43)
            assign(foot, material("Rubber", base=(0.02, 0.02, 0.02), rough=0.9))

    scale = max(W, H) + 1.6
    camera_top(scale, px, py, front=True)

    # Body (everything but the panel).
    assign(pnl, hold)
    render(os.path.join(OUT, f"{kind}_body.png"))

    # Panel layers for tinting.
    for o in bpy.context.scene.objects:
        if o.type in ("MESH", "CURVE") and o.name != "Panel":
            o.hide_render = True
    assign(pnl, white)
    render(os.path.join(OUT, f"{kind}_panel_shade.png"))
    assign(pnl, gloss)
    render(os.path.join(OUT, f"{kind}_panel_spec.png"))

    def rect(r):
        return front_rect(r[0], r[1], r[2], r[3], scale, px, py)

    write_json(kind, {
        "size": [px, py],
        "body": rect((-W / 2, -H / 2, W / 2, H / 2)),
        "panel": rect(panel),
        "grille": rect(grille_r),
    })


# ---------------------------------------------------------------------------

ASSETS = {
    "enclosure": render_enclosure,
    "knob": render_knob,
    "footswitch": render_footswitch,
    "led": render_led,
    "board": render_board,
    "floor": render_floor,
    "amp": lambda: render_rig("amp"),
    "cab": lambda: render_rig("cab"),
}

if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else ["all"]
    names = list(ASSETS) if "all" in args else args
    for n in names:
        ASSETS[n]()
