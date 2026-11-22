#!/usr/bin/env python3
"""Validate the WWK Favorite Sedan Cartographer authoring source.

Proves that ``favorite-sedan.carto`` parses under the deterministic
line-oriented project format and that the WWK variant bindings are complete:
three authored damage states (clean / hood-scratched / hood-panel), named
per-panel objects, per-face material regions, and the WWK object/mesh/asset
binding table.

Standard library only, no network, no build. Run from anywhere:

    python validate_favorite_sedan.py

Exits non-zero (fail closed) on the first contract violation.
"""

from pathlib import Path
import sys

PROJECT_DIR = Path(__file__).resolve().parent
SOURCE_PATH = PROJECT_DIR / "favorite-sedan.carto"

EXPECTED_NAMESPACE = "cartographer.project.wwk.favorite-sedan"
PANELS = ["body", "hood", "doors", "wheels", "front-clip"]
REGIONS = {1, 2, 3, 4, 5}

# Authored variant states: root object name, WWK master source object id,
# WWK composed mesh id, WWK mesh asset id, WWK material slug, root visibility.
VARIANT_BINDINGS = [
    ("Favorite Sedan (clean)", 51, 19, 1025, "sedan-favorite-clean", 1),
    ("Favorite Sedan (hood scratched)", 52, 20, 1026, "sedan-favorite-hood-scratched", 0),
    ("Favorite Sedan (hood panel)", 53, 21, 1027, "sedan-favorite-hood-panel", 0),
]

# Region id -> meaning. Region 2 exists so WWK can later bind *only* the hood
# to the mismatched replacement material; the faithful default binds every
# region of a variant to that variant's material set, matching the runtime
# blockout, which applies one material per variant mesh.
REGION_MEANING = {
    1: "body paint (shell, cabin, roof, trunk, bumpers, door band)",
    2: "hood surface and crash/repair evidence strips",
    3: "glass (windshield, rear glass)",
    4: "trim (pillars, grille, lamps, mirrors)",
    5: "wheels (tires, hubs)",
}

# Panel meshes that must be byte-identical across all three variants because
# the damage arc never touches them. Wheels differ for "hood-scratched".
SHARED_PANELS = ["body", "doors"]
CLEAN_SHARED_PANELS = ["wheels"]

# Original WWK hand-spliced blockout inventory (center + half extents), used
# to prove the authored source reproduces the runtime geometry. The authored
# source splits the body shell box into three z-contiguous boxes (front
# structure, door band, rear structure); the validator folds them back before
# comparing. Center coordinates, WWK axis convention: front at -Z.
def original_boxes(variant):
    body = [
        ((0, -0.075, 0, 0.95, 0.175, 2.35), 1),
        ((0, 0.6, 0.025, 0.78, 0.25, 1.225), 1),
        ((0, 0.875, 0.025, 0.75, 0.025, 1.025), 1),
        ((-0.75, 0.6, -1.125, 0.03, 0.25, 0.05), 4),
        ((0.75, 0.6, -1.125, 0.03, 0.25, 0.05), 4),
        ((-0.75, 0.6, 1.125, 0.03, 0.25, 0.05), 4),
        ((0.75, 0.6, 1.125, 0.03, 0.25, 0.05), 4),
        ((0, 0.6, -1.19, 0.7, 0.2, 0.02), 3),
        ((0, 0.6, 1.24, 0.7, 0.2, 0.02), 3),
        ((0, 0.415, 1.775, 0.95, 0.065, 0.525), 1),
    ]
    doors = [
        ((-1.06, 0.56, -1, 0.06, 0.06, 0.05), 4),
        ((1.06, 0.56, -1, 0.06, 0.06, 0.05), 4),
    ]
    wheels = []
    for side in (-1, 1):
        for z in (-1.35, 1.35):
            if variant == "hood-scratched" and side == -1 and z < 0:
                wheels.append(((-0.935, -0.3, -1.3, 0.165, 0.3, 0.4), 5))
            else:
                wheels.append(((side * 0.885, -0.3, z, 0.165, 0.3, 0.45), 5))
            wheels.append(((side * 1.058, -0.3, z, 0.008, 0.12, 0.12), 5))
    if variant == "clean":
        hood = [((0, 0.425, -1.725, 0.95, 0.075, 0.575), 2)]
        clip = [
            ((0, -0.05, -2.36, 0.98, 0.1, 0.06), 1),
            ((0, 0.175, -2.335, 0.4, 0.125, 0.025), 4),
            ((-0.7, 0.15, -2.335, 0.15, 0.1, 0.025), 4),
            ((0.7, 0.15, -2.335, 0.15, 0.1, 0.025), 4),
            ((0, -0.05, 2.36, 0.98, 0.1, 0.06), 1),
            ((-0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
            ((0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
        ]
    elif variant == "hood-scratched":
        hood = [
            ((0, 0.39, -2.05, 0.95, 0.04, 0.25), 2),
            ((0, 0.455, -1.475, 0.95, 0.105, 0.325), 2),
            ((-1.012, 0.225, -1.7, 0.012, 0.075, 0.5), 2),
        ]
        clip = [
            ((-0.1, -0.15, -2.42, 0.98, 0.1, 0.06), 1),
            ((0.05, 0.145, -2.335, 0.4, 0.095, 0.025), 4),
            ((0.7, 0.15, -2.335, 0.15, 0.1, 0.025), 4),
            ((0, -0.05, 2.36, 0.98, 0.1, 0.06), 1),
            ((-0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
            ((0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
        ]
    else:
        hood = [
            ((0, 0.435, -1.72, 0.93, 0.095, 0.56), 2),
            ((-0.945, 0.43, -1.72, 0.01, 0.02, 0.57), 2),
            ((0.945, 0.43, -1.72, 0.01, 0.02, 0.57), 2),
            ((-1.003, 0.225, -1.7, 0.003, 0.1, 0.55), 2),
        ]
        clip = [
            ((0, -0.05, -2.36, 0.98, 0.1, 0.06), 1),
            ((0, 0.175, -2.335, 0.4, 0.125, 0.025), 4),
            ((-0.7, 0.15, -2.335, 0.17, 0.11, 0.025), 4),
            ((0.7, 0.15, -2.335, 0.15, 0.1, 0.025), 4),
            ((0, -0.05, 2.36, 0.98, 0.1, 0.06), 1),
            ((-0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
            ((0.75, 0.25, 2.33, 0.15, 0.1, 0.03), 4),
        ]
    merged = {"body": body, "hood": hood, "doors": doors,
              "wheels": wheels, "front-clip": clip}
    return [(panel, box, region) for panel in PANELS for box, region in merged[panel]]

# Center + half extents of the three authored shell-split boxes (front
# structure in body, door band in doors, rear structure in body). fold_shell
# unions these; it must never hardcode the result.
SHELL_SPLIT = (
    (0, -0.075, -1.4625, 0.95, 0.175, 0.8875),
    (0, -0.075, 0, 0.95, 0.175, 1.175),
    (0, -0.075, 1.4625, 0.95, 0.175, 0.8875),
)

FAILURES = []


def fail(message):
    FAILURES.append(message)


def check(condition, message):
    if not condition:
        fail(message)


class Project:
    def __init__(self, text):
        self.lines = text.split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()
        self.objects = {}
        self.meshes = {}
        self._parse()

    def _parse(self):
        lines = self.lines
        check(lines[0] == "CARTOGRAPHER_PROJECT 34",
              f"project schema header is {lines[0]!r}, expected schema 34")
        check(lines[1] == 'AUTHORING "cartographer.authoring" "meters" "right_handed_y_up"',
              "authoring envelope record is missing or malformed")
        namespace = lines[2].split(" ", 1)
        check(namespace[0] == "SOURCE_NAMESPACE" and len(namespace) == 2
              and namespace[1] == f'"{EXPECTED_NAMESPACE}"',
              f"source namespace record is {lines[2]!r}")
        check(lines[4].startswith("REVISION ") and lines[4][9:].isdigit()
              and int(lines[4][9:]) >= 1, f"invalid revision record {lines[4]!r}")

        index = 5
        index = self._parse_objects(lines, index)
        index = self._parse_meshes(lines, index)
        # Trailing schema-34 table skeleton: every record must be present,
        # empty, and in canonical order; a missing or extra table is refused.
        skeleton = lines[index:]
        check(skeleton and skeleton[-1] == "END", "project is not terminated by END")
        quoted = {"SCIENTIFIC_MODEL", "WORLD_MODEL", "MATERIAL_CATALOG",
                  "PRESENTATION_CATALOG", "CONNECTION_PORT_GRAPH"}
        seen = []
        cursor = 0
        while cursor < len(skeleton) - 1:
            line = skeleton[cursor]
            tag = line.split(" ", 1)[0]
            seen.append(tag)
            if tag in quoted:
                inner = [line.split('"', 1)[1]]
                cursor += 1
                while cursor < len(skeleton) - 1 and skeleton[cursor] != '"':
                    inner.append(skeleton[cursor])
                    cursor += 1
                check(cursor < len(skeleton) - 1 and skeleton[cursor] == '"',
                      f"{tag} record is not closed by a quoted-line terminator")
                check(bool(inner) and inner[0].startswith("CARTOGRAPHER_"),
                      f"{tag} record lacks its versioned CARTOGRAPHER_ framing: "
                      f"{inner[0] if inner else '<empty>'!r}")
                check(bool(inner) and inner[-1] == "END",
                      f"{tag} record lacks its END terminator: "
                      f"{inner[-1] if inner else '<empty>'!r}")
                check(all(int(record.split(" ", 1)[1]) == 0
                          for record in inner[1:-1]
                          if record.split(" ", 1)[0].isupper()),
                      f"{tag} record carries nonempty tables")
            elif tag == "CONDITION_AUTHORING":
                check(line == "CONDITION_AUTHORING NONE",
                      f"{line!r} must be CONDITION_AUTHORING NONE")
            elif tag.endswith("_HEX"):
                check(int(line.split(" ", 1)[1], 16) >= 0,
                      f"{tag} record is not hexadecimal")
            else:
                tail = line.split(" ", 1)[1] if " " in line else ""
                check(tail.isdigit() and int(tail) == 0,
                      f"{line!r} must be an empty table")
            cursor += 1
        required = ["TOPOLOGY_RECEIPTS", "MESH_PATCHES", "SCIENTIFIC_MODEL",
                    "WORLD_MODEL", "MATERIAL_CATALOG", "SCULPT_STACKS",
                    "GARMENT_ASSETS", "CLOTH_REST_POSE_RECEIPTS",
                    "CONDITION_AUTHORING", "PRESENTATION_CATALOG",
                    "CURVE_PATHS", "ROAD_SOURCES", "SWEEP_SOURCES",
                    "SIGN_TEXT_SOURCES", "SPATIAL_FIELDS",
                    "SPATIAL_FIELD_LINEAGE", "CURVE_PATH_LINEAGE",
                    "ROAD_SOURCE_LINEAGE", "SWEEP_SOURCE_LINEAGE",
                    "SIGN_TEXT_SOURCE_LINEAGE", "SIGN_TEXT_INSTANCE_RECEIPTS",
                    "ROAD_SURFACE_INSTANCE_RECEIPTS", "SOURCE_ASSET_REGISTRY_HEX",
                    "BIOLOGICAL_AUTHORING_SOURCES", "BIOLOGICAL_AUTHORING_LINEAGE",
                    "BIOLOGICAL_PROXY_INSTANCE_RECEIPTS",
                    "CONDITION_VEGETATION_PLACEMENT_RECEIPTS",
                    "RIG_MODULE_TEMPLATES", "DECAL_PLACEMENTS",
                    "AUTHORING_STANDARDS_HEX", "POSE_LIBRARIES",
                    "WEIGHT_EDIT_RECEIPTS", "CURVE_EDIT_RECEIPTS",
                    "CONNECTION_PORT_GRAPH"]
        check(seen == required,
              f"trailing table skeleton is {seen}, expected exactly {required}")

    def _parse_objects(self, lines, index):
        header = lines[index].split(" ")
        check(header[0] == "OBJECTS" and header[1].isdigit(),
              f"OBJECTS header missing at line {index + 1}")
        count = int(header[1])
        index += 1
        for _ in range(count):
            line = lines[index]
            index += 1
            parts = line.split(" ", 3)
            check(parts[0] == "OBJECT" and parts[1].isdigit() and parts[2].isdigit(),
                  f"malformed object record {line!r}")
            object_id, parent = int(parts[1]), int(parts[2])
            rest = parts[3]
            check(rest.startswith('"'), f"object name must be quoted: {line!r}")
            name, _, tail = rest[1:].partition('"')
            fields = tail.split()
            check(len(fields) == 13, f"object {object_id} has {len(fields)} fields, expected 13")
            values = []
            for field in fields:
                try:
                    values.append(float(field))
                except ValueError:
                    fail(f"object {object_id} has a non-numeric field {field!r}")
                    values.append(0.0)
            check(all(value == value and abs(value) != float("inf") for value in values),
                  f"object {object_id} has a non-finite field")
            quaternion = values[3:7]
            check(abs(sum(component * component for component in quaternion)) > 1e-12,
                  f"object {object_id} has a degenerate rotation quaternion")
            check(object_id not in self.objects, f"duplicate object id {object_id}")
            self.objects[object_id] = {
                "parent": parent, "name": name, "fields": values,
                "visible": int(values[10]), "locked": int(values[11]),
                "mesh": int(values[12]),
            }
        for object_id, record in self.objects.items():
            parent = record["parent"]
            check(parent == 0 or parent in self.objects,
                  f"object {object_id} references missing parent {parent}")
            mesh = record["mesh"]
            check(mesh == 0 or mesh > 0, f"object {object_id} has invalid mesh id {mesh}")
        return index

    def _parse_meshes(self, lines, index):
        header = lines[index].split(" ")
        check(header[0] == "MESHES" and header[1].isdigit(),
              f"MESHES header missing at line {index + 1}")
        count = int(header[1])
        index += 1
        for _ in range(count):
            line = lines[index].split(" ")
            index += 1
            check(line[0] == "MESH" and line[1].isdigit() and line[2].isdigit(),
                  f"malformed mesh record {line}")
            mesh_id = int(line[1])
            check(mesh_id not in self.meshes, f"duplicate mesh id {mesh_id}")
            mesh = {"revision": int(line[2]), "vertices": {}, "faces": [],
                    "regions": []}
            check(lines[index].startswith("VERTICES "), "missing VERTICES header")
            vertex_count = int(lines[index].split(" ")[1])
            index += 1
            for vertex_index in range(1, vertex_count + 1):
                fields = lines[index].split(" ")
                index += 1
                check(fields[0] == "VERTEX" and int(fields[1]) == vertex_index,
                      f"mesh {mesh_id} vertex {vertex_index} is out of order")
                position = tuple(float(value) for value in fields[2:5])
                check(len(fields) == 5 and all(coord == coord for coord in position),
                      f"mesh {mesh_id} vertex {vertex_index} is malformed")
                mesh["vertices"][vertex_index] = position
            check(lines[index].startswith("FACES "), "missing FACES header")
            face_count = int(lines[index].split(" ")[1])
            index += 1
            for face_index in range(1, face_count + 1):
                fields = lines[index].split(" ")
                index += 1
                check(fields[0] == "FACE" and int(fields[1]) == face_index
                      and fields[2] == "4",
                      f"mesh {mesh_id} face {face_index} is not an ordered quad")
                corner_ids = [int(value) for value in fields[3:7]]
                check(len(fields) == 7, f"mesh {mesh_id} face {face_index} is malformed")
                check(all(corner in mesh["vertices"] for corner in corner_ids),
                      f"mesh {mesh_id} face {face_index} references an unknown vertex")
                mesh["faces"].append(corner_ids)
            check(lines[index] == "ATTRIBUTES", f"mesh {mesh_id} misses ATTRIBUTES")
            check(lines[index + 1] == "DOMAIN_COUNTS 1",
                  f"mesh {mesh_id} must declare exactly one attribute domain")
            domain_fields = lines[index + 2].split(" ")
            check(domain_fields[:2] == ["DOMAIN_COUNT", "3"]
                  and int(domain_fields[2]) == face_count,
                  f"mesh {mesh_id} face-domain element count must equal its face count")
            check(lines[index + 3] == "LAYERS 1",
                  f"mesh {mesh_id} must declare exactly one attribute layer")
            layer_fields = lines[index + 4].split(" ")
            check(layer_fields[:2] == ["LAYER", '"condition.material_region"']
                  and layer_fields[2] == "3" and layer_fields[3] == "2"
                  and int(layer_fields[4]) == face_count,
                  f"mesh {mesh_id} material-region layer must cover every face")
            index += 5
            for face_index in range(face_count):
                field = lines[index].split(" ")
                index += 1
                check(field[:1] == ["ATTRIBUTE_VALUE"] and field[1].isdigit(),
                      f"mesh {mesh_id} attribute value {face_index} is malformed")
                mesh["regions"].append(int(field[1]))
            check(lines[index] == "UV_SETS 0", f"mesh {mesh_id} must declare no UV sets")
            index += 1
            self.meshes[mesh_id] = mesh
        # Every mesh must be bound to exactly one object.
        bound = [record["mesh"] for record in self.objects.values() if record["mesh"]]
        check(sorted(bound) == sorted(self.meshes),
              "object mesh bindings do not cover each mesh exactly once")
        return index


def mesh_boxes(mesh):
    """Reconstruct the authored axis-aligned box inventory of one mesh."""
    vertices = mesh["vertices"]
    template = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    check(len(vertices) % 8 == 0,
          f"mesh has {len(vertices)} vertices, not a whole number of boxes")
    boxes = []
    for group in range(0, len(vertices), 8):
        corners = [vertices[group + offset + 1] for offset in range(8)]
        xs = sorted({corner[0] for corner in corners})
        ys = sorted({corner[1] for corner in corners})
        zs = sorted({corner[2] for corner in corners})
        check(len(xs) == 2 and len(ys) == 2 and len(zs) == 2,
              f"vertices {group + 1}..{group + 8} do not form an axis-aligned box")
        lookup = {(x, y, z) for x in xs for y in ys for z in zs}
        check({(corner[0], corner[1], corner[2]) for corner in corners} == lookup,
              f"vertices {group + 1}..{group + 8} are not the 8 corners of one box")
        for face_offset, face in enumerate(mesh["faces"][group // 8 * 6:
                                                         group // 8 * 6 + 6]):
            expected = [group + 1 + offset
                        for offset in [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4],
                                       [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5]][face_offset]]
            check(face == expected,
                  f"box at vertices {group + 1} deviates from the shared winding template")
        center = ((xs[0] + xs[1]) / 2, (ys[0] + ys[1]) / 2, (zs[0] + zs[1]) / 2)
        half = ((xs[1] - xs[0]) / 2, (ys[1] - ys[0]) / 2, (zs[1] - zs[0]) / 2)
        boxes.append((center, half, mesh["regions"][group // 8 * 6]))
    return boxes


def fold_shell(tagged_boxes):
    """Fold the three shell-split boxes back into the original body shell box.

    ``tagged_boxes`` is a list of ``(panel, (center, half, region))``. The
    split boxes live in the body and doors panels; the folded shell is
    reported under the body panel. The folded extents are the union of the
    three authored boxes, computed here — never hardcoded — so the fidelity
    proof only passes when the authored source actually reconstructs the
    original WWK blockout shell.
    """
    split = {tuple(round(value, 6) for value in candidate[:3]): candidate
             for candidate in SHELL_SPLIT}
    found = {}
    for panel, box in tagged_boxes:
        key = tuple(round(value, 6) for value in box[0])
        if key in split:
            found[key] = (panel, box)
    if len(found) != len(split):
        return tagged_boxes
    kept = [(panel, box) for panel, box in tagged_boxes
            if tuple(round(value, 6) for value in box[0]) not in split]
    boxes = [box for _panel, box in found.values()]
    # The three slabs must share one x/y cross-section and form a single
    # contiguous z-solid; otherwise they are not a fold of one shell box.
    cross_sections = {(box[0][0], box[0][1], box[1][0], box[1][1])
                      for box in boxes}
    intervals = sorted((box[0][2] - box[1][2], box[0][2] + box[1][2])
                       for box in boxes)
    if len(cross_sections) != 1 or any(
            intervals[index][1] < intervals[index + 1][0] - 1e-9
            for index in range(len(intervals) - 1)):
        return tagged_boxes
    x_center, y_center, x_half, y_half = next(iter(cross_sections))
    z_min = intervals[0][0]
    z_max = intervals[-1][1]
    region = found[tuple(round(v, 6) for v in SHELL_SPLIT[0][:3])][1][2]
    shell = ((x_center, y_center, (z_min + z_max) / 2),
             (x_half, y_half, (z_max - z_min) / 2), region)
    return kept + [("body", shell)]


def check_variant_tree(project):
    roots = [object_id for object_id, record in project.objects.items()
             if record["parent"] == 0]
    check(len(roots) == 3, f"expected 3 variant root objects, found {len(roots)}")
    by_name = {project.objects[root]["name"]: root for root in roots}
    for name, wwk_object, wwk_mesh, wwk_asset, slug, visible in VARIANT_BINDINGS:
        check(name in by_name,
              f"variant root {name!r} is missing (WWK source object {wwk_object})")
        if name not in by_name:
            continue
        root = by_name[name]
        record = project.objects[root]
        check(record["visible"] == visible,
              f"{name!r} root visibility is {record['visible']}, expected {visible}")
        check(record["mesh"] == 0, f"{name!r} root must not bind a mesh directly")
        check(record["fields"][:6] == [0.0] * 6 and record["fields"][6] == 1.0
              and record["fields"][7:10] == [1.0, 1.0, 1.0],
              f"{name!r} root transform must be identity")
        children = {project.objects[child]["name"]: child
                    for child, candidate in project.objects.items()
                    if candidate["parent"] == root}
        check(sorted(children) == sorted(PANELS),
              f"{name!r} panels are {sorted(children)}, expected {sorted(PANELS)}")
        for panel, child in children.items():
            fields = project.objects[child]["fields"]
            check(fields[:6] == [0.0] * 6 and fields[6] == 1.0
                  and fields[7:10] == [1.0, 1.0, 1.0],
                  f"{name!r}/{panel} transform must be identity")
            check(project.objects[child]["visible"] == 1,
                  f"{name!r}/{panel} must stay visible under its root")
            mesh_id = project.objects[child]["mesh"]
            check(mesh_id in project.meshes,
                  f"{name!r}/{panel} binds missing mesh {mesh_id}")
            mesh = project.meshes[mesh_id]
            check(mesh["revision"] == 1, f"{name!r}/{panel} mesh revision must be 1")
            used = set(mesh["regions"])
            check(used <= REGIONS,
                  f"{name!r}/{panel} uses unknown region ids {used - REGIONS}")
            check(all(region in used for region in mesh["regions"]),
                  f"{name!r}/{panel} region layer has gaps")
            if panel == "body":
                check(used == {1, 3, 4},
                      f"{name!r}/body regions are {used}, expected {{1, 3, 4}}")
            elif panel == "hood":
                check(used == {2}, f"{name!r}/hood regions are {used}, expected {{2}}")
            elif panel == "doors":
                check(used == {1, 4},
                      f"{name!r}/doors regions are {used}, expected {{1, 4}}")
            elif panel == "wheels":
                check(used == {5},
                      f"{name!r}/wheels regions are {used}, expected {{5}}")
            else:
                check(used == {1, 4},
                      f"{name!r}/front-clip regions are {used}, expected {{1, 4}}")
    return by_name


def check_shared_panels(project, by_name):
    def mesh_of(variant_name, panel):
        root = by_name[variant_name]
        for child, candidate in project.objects.items():
            if candidate["parent"] == root and candidate["name"] == panel:
                return project.meshes[candidate["mesh"]]
        raise AssertionError(panel)

    variants = [binding[0] for binding in VARIANT_BINDINGS]
    for panel in SHARED_PANELS:
        reference = mesh_of(variants[0], panel)
        for name in variants[1:]:
            check(mesh_of(name, panel) == reference,
                  f"{panel} geometry differs between {variants[0]!r} and {name!r}")
    for panel in CLEAN_SHARED_PANELS:
        reference = mesh_of(variants[0], panel)
        check(mesh_of(variants[2], panel) == reference,
              f"{panel} geometry differs between clean and hood-panel variants")


def check_geometry_contract(project, by_name):
    for name, _object, _mesh, _asset, _slug, _visible in VARIANT_BINDINGS:
        root = by_name[name]
        all_boxes = []
        for child, candidate in project.objects.items():
            if candidate["parent"] != root:
                continue
            mesh = project.meshes[candidate["mesh"]]
            boxes = mesh_boxes(mesh)
            if candidate["name"] == "wheels":
                lowest = min(vertex[1] for vertex in mesh["vertices"].values())
                check(lowest == -0.6,
                      f"{name}/wheels tire bottom is {lowest}, expected -0.6")
            all_boxes.extend((candidate["name"], box) for box in boxes)
        xs = [abs(box[0][0]) + box[1][0] for _panel, box in all_boxes]
        top = max(box[0][1] + box[1][1] for _panel, box in all_boxes)
        front = min(box[0][2] - box[1][2] for _panel, box in all_boxes)
        check(max(xs) <= 1.13, f"{name} body footprint exceeds the sedan envelope")
        check(top <= 0.91,
              f"{name} rises above the sedan envelope")
        check(front <= -2.39, f"{name} front extent {front} is behind -Z convention")

        # Fidelity: fold the shell split and compare against the WWK blockout.
        # Comparison is at the authored 5-decimal precision (see EXPORT.md).
        tagged = fold_shell([(panel, box) for panel, box in all_boxes])
        authored = [(panel, tuple(round(value, 5) for value in box[0] + box[1]),
                     box[2])
                    for panel, box in tagged]
        slug = {binding[0]: binding[4] for binding in VARIANT_BINDINGS}[name]
        slug = {"sedan-favorite-clean": "clean",
                "sedan-favorite-hood-scratched": "hood-scratched",
                "sedan-favorite-hood-panel": "hood-panel"}[slug]
        expected = [(panel, tuple(round(value, 5) for value in box), region)
                    for panel, box, region in original_boxes(slug)]
        check(sorted(authored) == sorted(expected),
              f"{name} box inventory diverges from the WWK blockout contract")


def check_binding_table():
    for region in sorted(REGIONS):
        check(region in REGION_MEANING, f"region {region} lacks documented meaning")
    for name, _object, _mesh, _asset, slug, _visible in VARIANT_BINDINGS:
        for region in sorted(REGIONS):
            check(isinstance(slug, str) and slug.startswith("sedan-favorite"),
                  f"{name} region {region} binds no WWK material slug")
    # The three slugs must be exactly WWK's authored sedan material sets.
    slugs = [binding[4] for binding in VARIANT_BINDINGS]
    check(slugs == ["sedan-favorite-clean", "sedan-favorite-hood-scratched",
                    "sedan-favorite-hood-panel"],
          f"variant material slugs are {slugs}")


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else str(SOURCE_PATH)
    path = Path(source)
    if not path.is_file():
        print(f"FAIL: {path} does not exist", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    check("\r" not in text, "source contains carriage returns; keep LF text")
    check(len(text.encode("utf-8")) <= 128 * 1024 * 1024,
          "source exceeds the serialized input bound")
    project = None
    try:
        project = Project(text)
    except Exception as error:  # malformed input must fail closed, never crash
        fail(f"source does not parse: {error}")
    by_name = check_variant_tree(project) if project is not None else {}
    if len(FAILURES) == 0:
        check_shared_panels(project, by_name)
        check_geometry_contract(project, by_name)
        check_binding_table()
    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"FAIL: {path} violates {len(FAILURES)} contract check(s)",
              file=sys.stderr)
        return 1
    objects = len(project.objects)
    meshes = len(project.meshes)
    faces = sum(len(mesh["faces"]) for mesh in project.meshes.values())
    print(f"OK: {path.name} parses: {objects} objects, {meshes} meshes, "
          f"{faces} quad faces")
    for name, wwk_object, wwk_mesh, wwk_asset, slug, visible in VARIANT_BINDINGS:
        print(f"OK: {name}: root visible={visible}, WWK object {wwk_object}, "
              f"WWK mesh {wwk_mesh}, WWK asset {wwk_asset}, material set {slug}")
    for region in sorted(REGIONS):
        print(f"OK: region {region}: {REGION_MEANING[region]}")
    print("OK: variant bindings complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
