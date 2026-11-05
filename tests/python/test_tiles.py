import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
import pytest

from ponderosa.extract import read_extract
from ponderosa.geo import Area, select_tiles, tile_north_west, tile_of
from ponderosa.mesh import Mesh, clean_ring, extrude, points_in_ring, ribbon_quads, signed_area
from ponderosa.osm import Feature, OsmData, _join
from ponderosa.tiles import (GRASS, HEADER, MAGIC, OVERVIEW_HEADER, OVERVIEW_MAGIC, Dem, build_all, build_overview,
                             overview_cell, parse_length, read_manifest, tile_name)


class FlatDem:
    def height(self, east, north):
        return np.asarray(east, np.float64) * 0.0 + 1500.0 + (np.asarray(east, np.float64) - 1000.0) * 0.01


AREA = Area("test", 0.0, 0.0, 100.0, 2, 10.0)


def test_tile_names_do_not_depend_on_the_area():
    assert tile_of(1000.0, 2000.0, 500) == (2, 4)
    assert tile_of(999.9, 1999.9, 500) == (1, 3)
    assert tile_north_west(2, 4, 500) == (1000.0, 2500.0)


def test_area_is_a_square_of_tiles(monkeypatch):
    monkeypatch.setattr(Area, "centre_utm", lambda self: (1050.0, 2050.0))
    assert AREA.tiles() == [(9, 19), (10, 19), (9, 20), (10, 20)]
    assert AREA.bounds() == (900.0, 1900.0, 1100.0, 2100.0)


def test_ring_cleaning_and_area():
    ring = clean_ring([(0, 0), (0, 4), (4, 4), (4, 0), (0, 0)])
    assert len(ring) == 4 and signed_area(ring) == 16


def test_points_in_ring():
    ring = clean_ring([(0, 0), (4, 0), (4, 4), (0, 4)])
    assert points_in_ring(np.array([2.0, 5.0]), np.array([2.0, 2.0]), ring).tolist() == [True, False]


def test_flat_building_winds_outward_and_up():
    m = Mesh()
    extrude(m, clean_ring([(0, 0), (6, 0), (6, 4), (2, 6), (0, 4)]), 0, 5, (1, 2, 3), (4, 5, 6))
    p, n = np.array(m.positions), np.array(m.normals)
    for t in np.array(m.indices).reshape(-1, 3):
        face = np.cross(p[t[1]] - p[t[0]], p[t[2]] - p[t[0]])
        assert face @ n[t[0]] > 0
    assert (n[:, 1] > 0.9).any() and p[:, 1].max() == 5


def test_gabled_building_has_a_ridge_along_the_long_side():
    m = Mesh()
    extrude(m, clean_ring([(0, 0), (10, 0), (10, 6), (0, 6)]), 0, 3, (1, 2, 3), (4, 5, 6), rise=2)
    p = np.array(m.positions)
    ridge = p[p[:, 1] == 5]
    assert set(np.round(ridge[:, 0]).tolist()) == {0, 10} and set(ridge[:, 2].tolist()) == {-3}


def test_ribbon_is_as_wide_as_asked():
    quads = list(ribbon_quads(np.array([(0.0, 0.0), (10.0, 0.0)]), 3.0, step=2.0))
    assert len(quads) == 5 * 2
    ys = [p[1] for q in quads for p in q]
    assert min(ys) == pytest.approx(-1.5) and max(ys) == pytest.approx(1.5)


def test_ways_join_into_rings():
    assert _join([["a", "b"], ["b", "c"], ["c", "a"]])[0][0] == _join([["a", "b"], ["b", "c"], ["c", "a"]])[0][-1]
    assert _join([["a", "b"], ["b", "c"]]) == []


def test_parse_length():
    assert parse_length("12") == 12 and parse_length("30'") == pytest.approx(9.144) and parse_length("many") is None


def read_tile(path):
    data = path.read_bytes()
    h = HEADER.unpack_from(data)
    assert h[0] == MAGIC and h[1] == 1
    samples = h[6]
    heights = np.frombuffer(data, np.float32, (samples + 2) ** 2, HEADER.size).reshape(samples + 2, samples + 2)
    return h, heights


@pytest.fixture
def built(tmp_path, monkeypatch):
    from ponderosa.osm import Feature
    monkeypatch.setattr(Area, "centre_utm", lambda self: (1000.0, 2000.0))
    osm = OsmData()
    osm.areas.append(Feature("w1", {"building": "house"}, rings=[np.array([(950.0, 1950.0), (960.0, 1950.0), (960.0, 1956.0), (950.0, 1956.0), (950.0, 1950.0)])]))
    osm.lines.append(Feature("w2", {"highway": "residential"}, line=np.array([(910.0, 2010.0), (1090.0, 2010.0)])))
    osm.points.append(Feature("n3", {"natural": "tree"}, point=(1040.0, 1940.0)))
    summary = build_all(AREA, osm, FlatDem(), tmp_path)
    return tmp_path, summary


def test_every_tile_is_in_the_manifest_with_its_size(built):
    out, summary = built
    m = read_manifest(out / "manifest.txt")
    assert summary["tiles"] == 4 and len(m["tiles"]) == 4 and m["tile_size"] == "100"
    for (ix, iy), (size, _) in m["tiles"].items():
        assert (out / tile_name(ix, iy)).stat().st_size == size


def test_neighbouring_tiles_share_their_edge_heights(built):
    out, _ = built
    _, west = read_tile(out / tile_name(9, 19))
    _, east = read_tile(out / tile_name(10, 19))
    assert np.array_equal(west[1:-1, -2], east[1:-1, 1])
    assert np.array_equal(west[1:-1, -1], east[1:-1, 2])     # the border is the neighbour's first inner column


def test_a_road_crossing_a_tile_edge_lands_in_both(built):
    out, summary = built
    assert summary["buildings"] == 1 and summary["trees"] == 1
    meshes = [read_tile(out / tile_name(ix, 20))[0][7] for ix in (9, 10)]
    assert meshes == [1, 1]


def test_a_boundary_picks_tiles_that_are_not_a_rectangle():
    # An L: tile (5, 5) lies in the notch, more than the margin from the boundary.
    ring = np.array([[0.0, 0.0], [600.0, 0.0], [600.0, 200.0], [200.0, 200.0], [200.0, 600.0], [0.0, 600.0]])
    keys = select_tiles([ring], 100.0, 50.0)
    assert (0, 0) in keys and (5, 0) in keys and (0, 5) in keys
    assert (5, 5) not in keys and (3, 3) not in keys
    assert (1, 1) in keys                       # inside, touching no edge of the boundary
    assert (2, 2) in keys                       # outside, within the margin of the corner
    assert (-1, 0) in keys and (-2, 0) not in keys   # the margin reaches one tile out and no further
    assert keys == sorted(keys, key=lambda k: (k[1], k[0]))


def test_a_tile_inside_a_ring_is_kept_without_touching_it():
    ring = np.array([[0.0, 0.0], [1000.0, 0.0], [1000.0, 1000.0], [0.0, 1000.0]])
    assert (5, 5) in select_tiles([ring], 100.0, 0.0)


def test_an_area_within_a_boundary_uses_those_tiles():
    ring = np.array([[0.0, 0.0], [190.0, 0.0], [190.0, 90.0], [0.0, 90.0]])
    city = Area("city", 0.0, 0.0, 100.0, 0, 10.0).within([ring])
    assert city.tiles() == [(0, 0), (1, 0)] and city.bounds() == (0.0, 0.0, 200.0, 100.0)


def write_dem(path, east, north, heights):
    import tifffile
    tifffile.imwrite(path, np.asarray(heights, np.float32), extratags=[
        (33550, "d", 3, (2.0, 2.0, 0.0)), (33922, "d", 6, (0.0, 0.0, 0.0, east, north, 0.0))])


def test_dem_files_join_into_one_mosaic(tmp_path):
    write_dem(tmp_path / "a.tif", 100.0, 200.0, np.full((4, 4), 10.0))
    write_dem(tmp_path / "b.tif", 108.0, 200.0, np.full((4, 4), 20.0))
    write_dem(tmp_path / "c.tif", 100.0, 192.0, np.full((4, 4), 30.0))
    dem = Dem(tmp_path / "b.tif", tmp_path / "a.tif", tmp_path / "c.tif")
    assert float(dem.height(103.0, 197.0)) == 10.0 and float(dem.height(111.0, 197.0)) == 20.0
    assert float(dem.height(103.0, 189.0)) == 30.0
    assert 10.0 <= float(dem.height(111.0, 189.0)) <= 30.0     # the missing corner reads as the median


class RoughDem:
    sx = 2.0

    def height(self, east, north):
        return 1500.0 + 8.0 * np.sin(np.asarray(east, np.float64) / 37.0) * np.cos(np.asarray(north, np.float64) / 23.0)


def test_the_overview_stays_below_the_detailed_ground():
    area = Area("o", 0.0, 0.0, 100.0, 0, 10.0, overview_cell=20.0, keys=((0, 0), (1, 0), (0, 1)))
    dem = RoughDem()
    o = build_overview(area, OsmData(), [], dem, 1500.0)
    rows, cols = o.heights.shape
    assert (rows, cols) == (11, 11) and o.cell == 20.0
    e = np.linspace(1, 199, 300)
    n = np.linspace(1, 199, 300)
    ge, gn = np.meshgrid(e, n)
    fx, fy = ge / o.cell, (o.north - gn) / o.cell
    c, r = np.minimum(fx.astype(int), cols - 2), np.minimum(fy.astype(int), rows - 2)
    u, v = fx - c, fy - r
    h = o.heights
    surface = (h[r, c] * (1 - u) + h[r, c + 1] * u) * (1 - v) + (h[r + 1, c] * (1 - u) + h[r + 1, c + 1] * u) * v
    gap = dem.height(ge, gn) - 1500.0 - surface
    assert gap.min() > 0.5 and gap.max() < 12


def test_an_overview_too_large_for_one_terrain_tile_coarsens():
    area = Area("big", 0.0, 0.0, 500.0, 0, 2.0, overview_cell=20.0, keys=((0, 0), (49, 0)))
    assert overview_cell(area) == 25.0


def test_the_overview_tints_with_roofs_and_roads():
    area = Area("o", 0.0, 0.0, 100.0, 0, 10.0, overview_cell=20.0, keys=((0, 0),))
    road = Feature("w1", {"highway": "primary"}, line=np.array([[10.0, 50.0], [90.0, 50.0]]))
    roof = Feature("w2", {"building": "yes"}, rings=[np.array([[70.0, 10.0], [90.0, 10.0], [90.0, 30.0], [70.0, 30.0]])])
    o = build_overview(area, OsmData(areas=[roof], lines=[road]), [], RoughDem(), 1500.0)
    assert o.colours[0, 0].tolist() == list(GRASS)     # nothing near it
    assert o.colours[2, 2].tolist() != list(GRASS)     # the road
    assert o.colours[4, 4].tolist() != list(GRASS)     # the roof


def test_the_overview_file_and_manifest_line(tmp_path):
    area = Area("test", 0.0, 0.0, 100.0, 2, 10.0, overview_cell=20.0)
    assert build_all(area, OsmData(), RoughDem(), tmp_path)["overview_bytes"] > 0
    manifest = read_manifest(tmp_path / "manifest.txt")
    assert manifest["overview"] == "overview.bin"
    data = (tmp_path / "overview.bin").read_bytes()
    magic, version, cols, rows, cell, east, north = OVERVIEW_HEADER.unpack_from(data)
    assert magic == OVERVIEW_MAGIC and version == 1 and cell == 20.0
    assert len(data) == OVERVIEW_HEADER.size + cols * rows * 4 + ((cols * rows * 3 + 3) // 4) * 4


def test_sampling_writes_only_some_tiles(tmp_path):
    area = Area("test", 0.0, 0.0, 100.0, 4, 10.0)
    summary = build_all(area, OsmData(), RoughDem(), tmp_path, sample=4)
    assert summary["tiles"] == 16 and summary["written"] == 4
    assert len(list(tmp_path.glob("*.tile"))) == 4


def test_an_extract_keeps_geometry_but_not_names_or_addresses(tmp_path):
    osmium = pytest.importorskip("osmium")
    path = tmp_path / "x.osm.pbf"
    with osmium.SimpleWriter(str(path)) as w:
        square = [(1, -105.0, 40.0), (2, -104.999, 40.0), (3, -104.999, 40.001), (4, -105.0, 40.001)]
        for ref, lon, lat in square:
            w.add_node(osmium.osm.mutable.Node(id=ref, location=(lon, lat), tags={"addr:street": "Sample Street"}))
        w.add_node(osmium.osm.mutable.Node(id=5, location=(-105.0005, 40.0005), tags={"natural": "tree", "name": "Old Oak"}))
        w.add_node(osmium.osm.mutable.Node(id=6, location=(-90.0, 30.0), tags={"natural": "tree"}))
        w.add_way(osmium.osm.mutable.Way(id=10, nodes=[1, 2, 3, 4, 1], tags={"building": "yes", "name": "Sample Hall",
                                                                             "addr:housenumber": "12"}))
        w.add_way(osmium.osm.mutable.Way(id=11, nodes=[1, 2, 3], tags={}))
        w.add_way(osmium.osm.mutable.Way(id=12, nodes=[3, 4, 1], tags={}))
        w.add_relation(osmium.osm.mutable.Relation(id=20, members=[("w", 11, "outer"), ("w", 12, "outer")], tags={
            "type": "multipolygon", "landuse": "grass", "name": "Sample Green"}))
    data = read_extract(path, -105.01, 39.99, -104.99, 40.01)
    assert len(data.areas) == 2
    building = next(a for a in data.areas if "building" in a.tags)
    assert building.tags == {"building": "yes"} and len(building.rings[0]) == 5
    green = next(a for a in data.areas if "landuse" in a.tags)
    assert green.tags == {"type": "multipolygon", "landuse": "grass"} and len(green.rings[0]) == 5
    assert len(data.points) == 1 and data.points[0].tags == {"natural": "tree"}


def test_a_resumed_build_keeps_finished_tiles_and_ignores_partial_ones(tmp_path):
    area = Area("test", 0.0, 0.0, 100.0, 2, 10.0)
    build_all(area, OsmData(), RoughDem(), tmp_path)
    kept = sorted(tmp_path.glob("*.tile"))[0]
    gone = sorted(tmp_path.glob("*.tile"))[1]
    kept.write_bytes(kept.read_bytes() + b"marker")
    gone.unlink()
    (tmp_path / (gone.stem + ".part123")).write_bytes(b"half a tile")
    summary = build_all(area, OsmData(), RoughDem(), tmp_path, resume=True)
    assert kept.read_bytes().endswith(b"marker") and gone.exists() and summary["written"] == 4
    assert read_manifest(tmp_path / "manifest.txt")["tiles"][tuple(map(int, gone.stem.split("_")))][0] == gone.stat().st_size


FULL_DOWNLOAD = """<?xml version="1.0"?>
<osm version="0.6" generator="an editor" copyright="c">
 <bounds minlat="1" minlon="1" maxlat="2" maxlon="2"/>
 <node id="1" lat="1.0" lon="1.0" user="someone" uid="7" changeset="9" timestamp="2020-01-01T00:00:00Z" version="3" visible="true"/>
 <node id="2" lat="1.0" lon="1.1" user="someone" uid="7"/>
 <node id="3" lat="1.1" lon="1.1" user="someone" uid="7"/>
 <node id="4" lat="1.1" lon="1.0" user="someone" uid="7">
  <tag k="addr:street" v="Example Street"/><tag k="name" v="A Cafe"/>
 </node>
 <node id="5" lat="1.2" lon="1.2" user="someone"><tag k="natural" v="tree"/><tag k="name" v="Old Oak"/></node>
 <node id="6" lat="1.3" lon="1.3" user="someone"><tag k="amenity" v="cafe"/><tag k="phone" v="+1 555 0100"/></node>
 <way id="10" user="someone" uid="7" changeset="9" timestamp="2020-01-01T00:00:00Z" version="2">
  <nd ref="1"/><nd ref="2"/><nd ref="3"/><nd ref="4"/><nd ref="1"/>
  <tag k="building" v="yes"/><tag k="height" v="9"/><tag k="name" v="A Shop"/><tag k="addr:housenumber" v="12"/>
  <tag k="website" v="https://example.com"/><tag k="email" v="a@example.com"/><tag k="contact:phone" v="1"/>
  <tag k="operator" v="Acme"/><tag k="brand" v="Acme"/><tag k="ref" v="7"/>
 </way>
 <way id="11" user="someone"><nd ref="1"/><nd ref="2"/><tag k="name" v="Nothing but a name"/></way>
 <relation id="20" user="someone" uid="7">
  <member type="way" ref="10" role="outer"/><member type="node" ref="6" role="label"/>
  <tag k="type" v="multipolygon"/><tag k="landuse" v="grass"/><tag k="name" v="A Park"/>
 </relation>
 <relation id="21" user="someone"><member type="way" ref="10" role="x"/><tag k="type" v="route"/></relation>
</osm>
"""


def test_sanitize_keeps_geometry_and_drops_everything_personal(tmp_path):
    from ponderosa.osm import parse
    from ponderosa.sanitize import KEEP_TAGS, sanitize

    source = tmp_path / "osm-0-0.osm"
    source.write_text(FULL_DOWNLOAD)
    out = tmp_path / "osm-sample.osm"
    assert sanitize([source], out) == {"nodes": 5, "ways": 1, "relations": 1}
    text = out.read_text()
    for gone in ("user=", "uid=", "changeset=", "timestamp=", "version=\"2\"", "version=\"3\"", "visible=", "bounds",
                 "someone", "addr:", "Example Street", "A Cafe", "A Shop", "A Park", "Old Oak", "phone", "555", "website",
                 "example.com", "email", "contact:", "operator", "brand", "Acme", "k=\"name\"", "k=\"ref\"", "an editor"):
        assert gone not in text, gone
    root = ET.fromstring(text)
    assert {t.get("k") for t in root.iter("tag")} <= KEEP_TAGS
    data = parse([out])
    buildings = [a for a in data.areas if a.tags.get("building") == "yes"]
    assert len(buildings) == 1 and buildings[0].tags["height"] == "9"
    assert any(p.tags.get("natural") == "tree" for p in data.points)


def test_sanitized_sample_holds_only_allowed_tags():
    from ponderosa.sanitize import KEEP_TAGS

    root = ET.parse(Path(__file__).resolve().parents[2] / "data" / "raw" / "osm-sample.osm").getroot()
    assert {t.get("k") for t in root.iter("tag")} <= KEEP_TAGS
    for el in root:
        assert set(el.attrib) <= {"id", "lat", "lon"}
