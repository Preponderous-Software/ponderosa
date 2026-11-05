"""ponderosa fetch | sanitize | verify | build"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .fetch import boundary_rings, fetch, fetch_city, sha256, verify, verify_city
from .geo import Area, utm_to_lonlat
from .osm import parse
from .sanitize import sanitize
from .tiles import Dem, build_all

ROOT = Path(__file__).resolve().parent.parent
BOX_MARGIN_M = 100.0   # roads and buildings that start just outside the tiles


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="ponderosa")
    ap.add_argument("command", choices=["fetch", "sanitize", "verify", "build"])
    ap.add_argument("--area", type=Path, default=ROOT / "area.json")
    ap.add_argument("--raw", type=Path, default=None, help="default: data/raw, or build/<area>-raw for a boundary area")
    ap.add_argument("--out", type=Path, default=None, help="default: build/tiles, or build/<area> for a boundary area")
    ap.add_argument("--fetched", type=Path, default=ROOT / "build" / "fetched", help="full downloads, never committed")
    ap.add_argument("--sources", type=Path, default=ROOT / "city" / "SOURCES.json")
    ap.add_argument("--sample", type=int, default=1, help="write every Nth tile, to measure a large area")
    ap.add_argument("--resume", action="store_true", help="keep tiles already written, after an interrupted build")
    args = ap.parse_args(argv)
    area = Area.load(args.area)
    city = area.boundary_relation != 0
    raw = args.raw or (ROOT / "build" / f"{area.name}-raw" if city else ROOT / "data" / "raw")
    out = args.out or (ROOT / "build" / (area.name if city else "tiles"))
    if args.command == "fetch":
        fetch_city(area, raw, args.sources) if city else fetch(area, args.fetched)
        return 0
    if args.command == "sanitize":
        target = raw / "osm-sample.osm"
        print(sanitize(sorted(args.fetched.glob("osm-*.osm")), target))
        sources = json.loads((raw / "SOURCES.json").read_text())
        sources[target.name] = {"sha256": sha256(target), "bytes": target.stat().st_size}
        (raw / "SOURCES.json").write_text(json.dumps(sources, indent=1) + "\n")
        return 0
    if args.command == "verify":
        bad = verify_city(raw, args.sources) if city else verify(raw)
        print("raw data OK" if not bad else f"mismatch: {bad}")
        return 1 if bad else 0
    if city:
        from .extract import read_extract
        area = area.within(boundary_rings(raw / "boundary.osm"))
        west, south, east, north = area.bounds()
        lon0, lat0 = utm_to_lonlat(west - BOX_MARGIN_M, south - BOX_MARGIN_M)
        lon1, lat1 = utm_to_lonlat(east + BOX_MARGIN_M, north + BOX_MARGIN_M)
        osm = read_extract(raw / "region.osm.pbf", lon0, lat0, lon1, lat1)
        dem = Dem(*sorted(raw.glob("dem-*.tif")))
    else:
        osm = parse(sorted(raw.glob("osm-*.osm")))
        dem = Dem(raw / "dem-3dep.tif")
    summary = build_all(area, osm, dem, out, args.sample, args.resume)
    print(f"wrote {out}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
