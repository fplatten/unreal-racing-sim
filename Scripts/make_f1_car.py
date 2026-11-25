"""
Builds the game's 1995-style open-wheel Formula car in Blender and exports it to SourceArt/Models/Car:

  f1car.glb            body (one object per part, material slots named for the game)
  f1_wheel_front.glb   front wheel: 13" rim, 0.30 m wide slick, 0.66 m diameter
  f1_wheel_rear.glb    rear wheel: 13" rim, 0.38 m wide slick

Run inside Blender (Text Editor > Run Script, or `blender -b -P Scripts/make_f1_car.py`).

Dimensions: 4.42 m (14.5 ft) long, 2.90 m wheelbase, 2.00 m wide over the tires, front track
1.69 m, rear 1.61 m; a raised nose with the front wing hung beneath it, a tall airbox over the
driver's head, sidepods that taper to a narrow gearbox, a two-element rear wing and a diffuser.
The paint comes from the game: material slots Livery (primary color), LiverySecondary,
LiveryAccent (the pinstripe between them) and Helmet are colored per car at run time.

Frame: the same as the game's other car models - Blender +X is the car's left, -Y forward, +Z
up, the origin on the ground under the center of mass (2.90 m wheelbase: front axle 1.60 m
ahead of it, rear axle 1.30 m behind). Wheels: the axle along X, the outer face toward +X.
"""
import math
import os

import bmesh
import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "SourceArt", "Models", "Car"))

FRONT_AXLE, REAR_AXLE = 1.60, -1.30
FRONT_HALF_TRACK, REAR_HALF_TRACK = 0.845, 0.805
WHEEL_R = 0.33
FRONT_TIRE_W, REAR_TIRE_W = 0.30, 0.38


def B(x, y, z):
    """Car frame (x forward, y left, z up) -> Blender."""
    return Vector((y, -x, z))


def car(v):
    """Blender -> car frame."""
    return Vector((-v.y, v.x, v.z))


# ------------------------------------------------------------------------------ materials

MATERIALS = {
    # name: (base color (linear), metallic, roughness)
    "Livery": ((0.05, 0.12, 0.55), 0.0, 0.3),
    "LiverySecondary": ((0.85, 0.85, 0.85), 0.0, 0.3),
    "LiveryAccent": ((0.75, 0.05, 0.05), 0.0, 0.3),
    "Helmet": ((0.9, 0.8, 0.1), 0.0, 0.15),
    "Visor": ((0.02, 0.02, 0.03), 0.3, 0.05),
    "Carbon": ((0.025, 0.025, 0.03), 0.2, 0.35),
    "Black": ((0.008, 0.008, 0.01), 0.0, 0.6),
    "Suspension": ((0.04, 0.04, 0.05), 0.5, 0.4),
    "Pad": ((0.03, 0.03, 0.04), 0.0, 0.9),
    "Light": ((1.0, 0.05, 0.03), 0.0, 0.3),
    "Tire": ((0.05, 0.05, 0.05), 0.0, 0.85),
    "Rim": ((0.12, 0.12, 0.13), 0.9, 0.3),
    "Lip": ((0.7, 0.7, 0.72), 1.0, 0.2),
    "Hub": ((0.55, 0.55, 0.57), 1.0, 0.25),
}


def material(name):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    col, metal, rough = MATERIALS[name]
    bsdf.inputs["Base Color"].default_value = (*col, 1.0)
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    if name == "Light":
        try:
            bsdf.inputs["Emission Color"].default_value = (1.0, 0.05, 0.03, 1.0)
            bsdf.inputs["Emission Strength"].default_value = 2.0
        except KeyError:
            pass
    return m


class Part:
    """One mesh object built with bmesh; faces carry material names."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.mats = []

    def mi(self, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        return self.mats.index(mat)

    def verts(self, pts):
        return [self.bm.verts.new(p) for p in pts]

    def face(self, vs, mat):
        f = self.bm.faces.new(vs)
        f.material_index = self.mi(mat)
        return f

    def finish(self, smooth_angle=None, collection=None):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=1e-5)
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.mats:
            me.materials.append(material(m))
        ob = bpy.data.objects.new(self.name, me)
        (collection or bpy.context.scene.collection).objects.link(ob)
        if smooth_angle is not None:
            for p in me.polygons:
                p.use_smooth = True
            try:
                me.set_sharp_from_angle(angle=math.radians(smooth_angle))
            except AttributeError:
                pass
        return ob


# ------------------------------------------------------------------------- shape helpers

def superellipse_ring(x, yc, w, zc, h, n, N):
    pts = []
    for i in range(N):
        t = 2 * math.pi * i / N
        c, s = math.cos(t), math.sin(t)
        y = yc + w * math.copysign(abs(c) ** (2.0 / n), c)
        z = zc + h * math.copysign(abs(s) ** (2.0 / n), s)
        pts.append(B(x, y, z))
    return pts


def loft(part, stations, paint, N=40, cap_front="Black", cap_back=None, yc=0.0):
    """stations: (x, halfwidth, top, bottom, exponent[, yc]); paint(center, s0, s1) -> material, with
    s0 / s1 the sine of the section angle at the face's two edges (+1 top, -1 bottom)."""
    rings = []
    for st in stations:
        x, w, top, bot, n = st[:5]
        c = st[5] if len(st) > 5 else yc
        rings.append(part.verts(superellipse_ring(x, c, w, (top + bot) / 2, (top - bot) / 2, n, N)))
    for a, b in zip(rings, rings[1:]):
        for i in range(N):
            j = (i + 1) % N
            vs = [a[i], a[j], b[j], b[i]]
            pts = [car(v.co) for v in vs]
            cen = sum(pts, Vector()) / 4
            # where round the section the face is: sin of the angles at its two edges (-1 bottom, +1 top)
            s0, s1 = math.sin(2 * math.pi * i / N), math.sin(2 * math.pi * (i + 1) / N)
            part.face(vs, paint(cen, s0, s1))
    part.face(rings[0], cap_front or paint(car(rings[0][0].co), 1, 1))
    part.face(list(reversed(rings[-1])), cap_back or paint(car(rings[-1][0].co), 1, 1))


def extrude_xz(part, profile, y0, y1, mat, side_mat=None):
    """A closed (x, z) outline, extruded across y0..y1 (wings, endplates, pylons)."""
    a = part.verts([B(x, y0, z) for x, z in profile])
    b = part.verts([B(x, y1, z) for x, z in profile])
    n = len(profile)
    for i in range(n):
        j = (i + 1) % n
        part.face([a[i], a[j], b[j], b[i]], mat)
    part.face(a, side_mat or mat)
    part.face(list(reversed(b)), side_mat or mat)


def extrude_xy(part, outline, z0, z1, mat):
    """A closed (x, y) outline, extruded from z0 to z1 (floor, diffuser fences)."""
    a = part.verts([B(x, y, z0) for x, y in outline])
    b = part.verts([B(x, y, z1) for x, y in outline])
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        part.face([a[i], a[j], b[j], b[i]], mat)
    part.face(a, mat)
    part.face(list(reversed(b)), mat)


def airfoil(x_le, x_te, z_le, z_te, thick, n=10):
    """Cambered wing section from the leading to the trailing edge, as an (x, z) outline."""
    top, bot = [], []
    for i in range(n + 1):
        t = i / n
        x = x_le + (x_te - x_le) * t
        zc = z_le + (z_te - z_le) * t + 0.06 * abs(x_te - x_le) * math.sin(math.pi * t)   # camber
        half = thick * (1.2 * math.sqrt(max(t, 1e-4)) * (1 - t) + 0.08)
        top.append((x, zc + half))
        bot.append((x, zc - half * 0.6))
    return top + list(reversed(bot[1:-1]))


def rod(part, p0, p1, r, mat, sides=8):
    p0, p1 = B(*p0), B(*p1)
    d = (p1 - p0).normalized()
    u = d.orthogonal().normalized()
    v = d.cross(u)
    a, b = [], []
    for i in range(sides):
        t = 2 * math.pi * i / sides
        o = (u * math.cos(t) + v * math.sin(t)) * r
        a.append(p0 + o)
        b.append(p1 + o)
    va, vb = part.verts(a), part.verts(b)
    for i in range(sides):
        j = (i + 1) % sides
        part.face([va[i], va[j], vb[j], vb[i]], mat)
    part.face(va, mat)
    part.face(list(reversed(vb)), mat)


def box(part, c, size, mat):
    x, y, z = c
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    outline = [(x - hx, y - hy), (x + hx, y - hy), (x + hx, y + hy), (x - hx, y + hy)]
    extrude_xy(part, outline, z - hz, z + hz, mat)


# --------------------------------------------------------------------------------- the car

def two_tone(split):
    """Primary paint above a line round each section (split: sine of its angle), secondary below,
    an accent pinstripe on the faces the line crosses."""
    def paint(c, s0, s1):
        if min(s0, s1) < split < max(s0, s1):
            return "LiveryAccent"
        return "Livery" if (s0 + s1) / 2 > split else "LiverySecondary"
    return paint


CHASSIS = [  # x, halfwidth, top, bottom, exponent: raised nose, cockpit, engine bay to the gearbox
    (2.33, 0.03, 0.365, 0.315, 2.0),
    (2.26, 0.07, 0.40, 0.28, 2.2),
    (2.05, 0.11, 0.45, 0.26, 2.4),
    (1.75, 0.15, 0.52, 0.24, 2.6),
    (1.45, 0.20, 0.60, 0.18, 3.0),
    (1.15, 0.25, 0.67, 0.10, 3.5),
    (0.85, 0.29, 0.71, 0.06, 4.0),
    (0.55, 0.30, 0.67, 0.05, 4.0),
    (0.10, 0.30, 0.66, 0.05, 4.0),
    (-0.30, 0.28, 0.60, 0.05, 4.0),
    (-0.80, 0.24, 0.52, 0.07, 4.0),
    (-1.25, 0.16, 0.44, 0.12, 3.5),
    (-1.60, 0.09, 0.38, 0.17, 3.0),
]
AIRBOX = [  # the engine cover with the air intake above the driver's head
    (0.10, 0.09, 0.94, 0.72, 2.4),
    (0.00, 0.14, 0.97, 0.58, 2.4),
    (-0.35, 0.21, 0.92, 0.42, 3.0),
    (-0.80, 0.22, 0.78, 0.30, 3.0),
    (-1.20, 0.15, 0.60, 0.25, 3.0),
    (-1.55, 0.09, 0.46, 0.22, 3.0),
]
SIDEPOD = [  # x, inner y, outer y, top, bottom, exponent
    (0.78, 0.29, 0.68, 0.50, 0.08, 4.0),
    (0.70, 0.29, 0.70, 0.53, 0.06, 4.0),
    (0.30, 0.29, 0.70, 0.54, 0.05, 4.0),
    (-0.20, 0.27, 0.66, 0.50, 0.05, 4.0),
    (-0.65, 0.24, 0.52, 0.42, 0.06, 3.5),
    (-1.05, 0.18, 0.32, 0.30, 0.08, 3.0),
]


def chassis_top(x):
    """Top of the tub at x (the cockpit opening sits on it)."""
    for a, b in zip(CHASSIS, CHASSIS[1:]):
        if b[0] <= x <= a[0]:
            t = (x - a[0]) / (b[0] - a[0])
            lerp = lambda i: a[i] + (b[i] - a[i]) * t
            return lerp(2), lerp(1), (lerp(2) - lerp(3)) / 2, lerp(4)
    return CHASSIS[-1][2], CHASSIS[-1][1], 0.2, 4.0


def build_body(col):
    body = Part("Chassis")
    loft(body, CHASSIS, two_tone(-0.15), N=48, cap_front="LiverySecondary", cap_back="Carbon")
    loft(body, AIRBOX, two_tone(-2.0), N=40, cap_front="Black", cap_back="Carbon")
    for side in (1, -1):
        st = [(x, (yo - yi) / 2, top, bot, n, side * (yo + yi) / 2) for x, yi, yo, top, bot, n in SIDEPOD]
        loft(body, st, two_tone(0.05), N=40, cap_front="Black", cap_back="Carbon")
    ob = body.finish(smooth_angle=50, collection=col)

    # cockpit opening: a dark recess draped over the tub, the padded rim round it
    cockpit = Part("Cockpit")
    rows, cols = 10, 16
    grid = []
    for i in range(rows + 1):
        x = 0.82 - (0.82 - 0.10) * i / rows
        top, w, h, n = chassis_top(x)
        half = 0.21 * math.sqrt(max(0.0, 1 - ((x - 0.46) / 0.36) ** 2)) + 0.005
        row = []
        for j in range(cols + 1):
            y = -half + 2 * half * j / cols
            drop = h * (1 - max(0.0, 1 - abs(y / w) ** n) ** (1 / n)) if w > 0 else 0
            row.append(B(x, y, top - drop + 0.004))
        grid.append(cockpit.verts(row))
    for i in range(rows):
        for j in range(cols):
            cockpit.face([grid[i][j], grid[i][j + 1], grid[i + 1][j + 1], grid[i + 1][j]], "Black")
    cockpit.finish(smooth_angle=60, collection=col)

    # the driver: helmet with a dark visor band
    head = Part("Driver")
    bmesh.ops.create_uvsphere(head.bm, u_segments=24, v_segments=16, radius=0.125,
                              matrix=__import__("mathutils").Matrix.Translation(B(0.34, 0.0, 0.76)))
    head.mi("Helmet")
    head.mi("Visor")
    for f in head.bm.faces:
        c = car(f.calc_center_median())
        f.material_index = 1 if (c.x > 0.40 and 0.73 < c.z < 0.80) else 0
    box(head, (0.20, 0.0, 0.72), (0.12, 0.30, 0.10), "Pad")   # headrest behind the helmet
    head.finish(smooth_angle=80, collection=col)

    # floor, step plank and diffuser
    floor = Part("Floor")
    extrude_xy(floor, [(1.05, -0.30), (1.05, 0.30), (0.80, 0.70), (-0.95, 0.70), (-1.05, 0.45),
                       (-1.05, -0.45), (-0.95, -0.70), (0.80, -0.70)], 0.035, 0.05, "Carbon")
    diffuser = [(-1.05, 0.05), (-1.72, 0.24), (-1.72, 0.255), (-1.05, 0.065)]
    extrude_xz(floor, diffuser, -0.45, 0.45, "Carbon")
    for y in (-0.42, -0.15, 0.15, 0.42):
        extrude_xz(floor, [(-1.05, 0.05), (-1.72, 0.24), (-1.72, 0.30), (-1.05, 0.10)], y - 0.006, y + 0.006, "Carbon")
    floor.finish(collection=col)

    # front wing hung under the nose: main plane, flaps, endplates, pylons
    fw = Part("FrontWing")
    extrude_xz(fw, airfoil(2.46, 2.10, 0.075, 0.095, 0.035), -0.70, 0.70, "LiverySecondary")
    for y0, y1 in ((0.22, 0.69), (-0.69, -0.22)):
        extrude_xz(fw, airfoil(2.18, 1.97, 0.115, 0.22, 0.025), y0, y1, "LiverySecondary")
    plate = [(2.47, 0.035), (1.94, 0.035), (1.92, 0.21), (2.00, 0.29), (2.30, 0.27), (2.47, 0.15)]
    for s in (1, -1):
        extrude_xz(fw, plate, s * 0.69, s * 0.705, "Livery")
        extrude_xz(fw, [(2.28, 0.09), (2.12, 0.09), (2.12, 0.30), (2.24, 0.33)], s * 0.07, s * 0.085, "Carbon")
    fw.finish(collection=col)

    # rear wing: endplates, the two-element top wing, the beam wing, the central pillar, the rain light
    rw = Part("RearWing")
    end = [(-1.42, 0.29), (-1.97, 0.29), (-1.99, 0.99), (-1.47, 0.99), (-1.42, 0.90)]
    for s in (1, -1):
        extrude_xz(rw, end, s * 0.485, s * 0.50, "Livery")
        extrude_xz(rw, [(-1.47, 0.985), (-1.99, 0.985), (-1.99, 0.995), (-1.47, 0.995)], s * 0.484, s * 0.501, "LiveryAccent")
    extrude_xz(rw, airfoil(-1.50, -1.78, 0.86, 0.90, 0.045), -0.485, 0.485, "LiverySecondary")
    extrude_xz(rw, airfoil(-1.76, -1.97, 0.90, 0.975, 0.03), -0.485, 0.485, "LiverySecondary")
    extrude_xz(rw, airfoil(-1.52, -1.76, 0.41, 0.44, 0.035), -0.485, 0.485, "Carbon")
    extrude_xz(rw, [(-1.55, 0.26), (-1.75, 0.26), (-1.70, 0.87), (-1.58, 0.87)], -0.008, 0.008, "Carbon")
    box(rw, (-1.76, 0.0, 0.30), (0.03, 0.10, 0.045), "Light")
    rw.finish(collection=col)

    # mirrors on stalks beside the cockpit
    mir = Part("Mirrors")
    for s in (1, -1):
        rod(mir, (0.80, s * 0.29, 0.64), (0.79, s * 0.45, 0.70), 0.008, "Carbon")
        box(mir, (0.78, s * 0.50, 0.72), (0.06, 0.13, 0.065), "Livery")
        box(mir, (0.748, s * 0.50, 0.72), (0.004, 0.11, 0.05), "Black")
    mir.finish(collection=col)

    # suspension: double wishbones, push rods and track rods, with uprights inside the wheels
    sus = Part("Suspension")
    r = 0.013
    for s in (1, -1):
        fx = FRONT_AXLE
        for a, b in (((1.75, 0.17, 0.50), (fx, 0.72, 0.46)), ((1.42, 0.20, 0.53), (fx, 0.72, 0.46)),
                     ((1.77, 0.14, 0.27), (fx, 0.75, 0.21)), ((1.42, 0.18, 0.21), (fx, 0.75, 0.21)),
                     ((1.50, 0.15, 0.58), (fx, 0.72, 0.23)), ((1.70, 0.16, 0.36), (1.70, 0.72, 0.34))):
            rod(sus, (a[0], s * a[1], a[2]), (b[0], s * b[1], b[2]), r, "Suspension")
        box(sus, (fx, s * 0.70, 0.33), (0.10, 0.03, 0.30), "Suspension")
        rx = REAR_AXLE
        for a, b in (((-1.10, 0.20, 0.40), (rx, 0.64, 0.46)), ((-1.45, 0.12, 0.40), (rx, 0.64, 0.46)),
                     ((-1.05, 0.22, 0.12), (rx, 0.66, 0.18)), ((-1.45, 0.12, 0.14), (rx, 0.66, 0.18)),
                     ((-1.50, 0.12, 0.25), (-1.50, 0.64, 0.25)), ((-1.15, 0.15, 0.42), (rx, 0.62, 0.19))):
            rod(sus, (a[0], s * a[1], a[2]), (b[0], s * b[1], b[2]), r, "Suspension")
        box(sus, (rx, s * 0.62, 0.33), (0.10, 0.03, 0.30), "Suspension")
    # exhausts and the gearbox casing between the rear wheels
    box(sus, (-1.45, 0.0, 0.30), (0.40, 0.16, 0.16), "Carbon")
    sus.finish(collection=col)
    return ob


# ------------------------------------------------------------------------------- the wheels

def lathe(part, profile, mat, segments=48, uv=True):
    """profile: (axial x, radius, v) points; turned round the X axis, u = angle / 360."""
    rings = []
    for k in range(segments + 1):
        a = 2 * math.pi * k / segments
        rings.append(part.verts([Vector((x, r * math.cos(a), r * math.sin(a))) for x, r, _ in profile]))
    faces = []
    for k in range(segments):
        for i in range(len(profile) - 1):
            f = part.face([rings[k][i], rings[k][i + 1], rings[k + 1][i + 1], rings[k + 1][i]], mat)
            faces.append((f, k, i))
    if uv:
        layer = part.bm.loops.layers.uv.verify()
        for f, k, i in faces:
            for loop, (kk, ii) in zip(f.loops, ((k, i), (k, i + 1), (k + 1, i + 1), (k + 1, i))):
                loop[layer].uv = (kk / segments, profile[ii][2])
    return rings


def build_wheel(name, width, col):
    hw = width / 2
    bead = 0.172
    # sidewall (v 0-0.3), tread (0.3-0.7), sidewall (0.7-1) as the game's tire texture expects;
    # 13" rims under a tall 1995 sidewall
    half = [(-hw * 0.86, bead, 0.0), (-hw * 0.97, 0.20, 0.075), (-hw, 0.245, 0.15), (-hw, 0.285, 0.225),
            (-hw * 0.97, 0.312, 0.30), (-hw * 0.88, 0.326, 0.34), (-hw * 0.6, 0.33, 0.40), (-hw * 0.25, 0.33, 0.46)]
    profile = half + [(0.0, 0.33, 0.5)] + [(-x, r, 1 - v) for x, r, v in reversed(half)]
    w = Part(name)
    lathe(w, profile, "Tire", segments=56)
    # rim: barrel, outer dish with spokes, polished lip, centre-lock hub
    lathe(w, [(hw * 0.84, 0.168, 0), (-hw * 0.84, 0.168, 0)], "Rim", segments=40, uv=False)
    lathe(w, [(hw * 0.84, 0.168, 0), (hw * 0.40, 0.150, 0), (hw * 0.30, 0.06, 0), (hw * 0.32, 0.0, 0)], "Rim", segments=40, uv=False)
    lathe(w, [(hw * 0.84, 0.162, 0), (hw * 0.90, 0.170, 0), (hw * 0.84, 0.176, 0)], "Lip", segments=40, uv=False)
    lathe(w, [(hw * 0.30, 0.05, 0), (hw * 0.55, 0.045, 0), (hw * 0.58, 0.0, 0)], "Hub", segments=16, uv=False)
    for k in range(6):
        a = 2 * math.pi * k / 6
        d = Vector((0.0, math.cos(a), math.sin(a)))
        n = Vector((0.0, -math.sin(a), math.cos(a)))
        x0, x1 = hw * 0.33, hw * 0.50
        pts = []
        for x in (x0, x1):
            for r0 in (0.055, 0.150):
                for t in (-0.012, 0.012):
                    pts.append(Vector((x, 0, 0)) + d * r0 + n * t)
        v = w.verts(pts)
        for q in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 2, 6, 4), (1, 5, 7, 3), (0, 4, 5, 1), (2, 3, 7, 6)):
            w.face([v[i] for i in q], "Rim")
    return w.finish(smooth_angle=35, collection=col)


# ---------------------------------------------------------------------------------- export

def uv_unwrap(obj, metres_per_unit=4.5):
    """Box projection (each face onto the plane its normal faces most), for the body detail maps."""
    if obj.data.uv_layers:
        return
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    layer = bm.loops.layers.uv.new("UVMap")
    for f in bm.faces:
        n = f.normal
        ax = max(range(3), key=lambda i: abs(n[i]))
        u_i, v_i = [(1, 2), (0, 2), (0, 1)][ax]
        for loop in f.loops:
            co = loop.vert.co
            loop[layer].uv = (co[u_i] / metres_per_unit + 0.5, co[v_i] / metres_per_unit + 0.5)
    bm.to_mesh(obj.data)
    bm.free()


def export(objs, path):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True,
                              export_yup=True, export_materials="EXPORT")
    print("exported", path)


def main(export_files=True):
    for c in list(bpy.data.collections):
        if c.name.startswith("F1Car"):
            for o in list(c.objects):
                bpy.data.objects.remove(o, do_unlink=True)
            bpy.data.collections.remove(c)
    col = bpy.data.collections.new("F1Car")
    bpy.context.scene.collection.children.link(col)
    build_body(col)
    body = [o for o in col.objects]
    wf = build_wheel("WheelFront", FRONT_TIRE_W, col)
    wr = build_wheel("WheelRear", REAR_TIRE_W, col)
    for o in body + [wf, wr]:
        uv_unwrap(o)
    # preview: wheels in place (the game places its own)
    if export_files:
        os.makedirs(OUT_DIR, exist_ok=True)
        export(body, os.path.join(OUT_DIR, "f1car.glb"))
        export([wf], os.path.join(OUT_DIR, "f1_wheel_front.glb"))
        export([wr], os.path.join(OUT_DIR, "f1_wheel_rear.glb"))
    tris = sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in body)
    print("body triangles", tris)
    return body, wf, wr


def preview_wheels(wf, wr):
    """Copies of the wheels at the four corners, for looking at the car in Blender."""
    col = bpy.data.collections["F1Car"]
    for src, x, ht in ((wf, FRONT_AXLE, FRONT_HALF_TRACK), (wr, REAR_AXLE, REAR_HALF_TRACK)):
        for s in (1, -1):
            o = src.copy()
            o.name = src.name + ("_L" if s > 0 else "_R") + "_preview"
            o.location = B(x, s * ht, WHEEL_R)
            if s < 0:
                o.rotation_euler = (0, 0, math.pi)
            col.objects.link(o)


if __name__ == "__main__":
    main()
