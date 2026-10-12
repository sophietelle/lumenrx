#!/usr/bin/env python3
import base64
import glob
import json
import sys

FMT = {
    "IntuosReportParser": "INTUOS",
    "IntuosV1ReportParser": "INTUOS_V1",
    "IntuosProReportParser": "INTUOS_V1",
    "CintiqV1ReportParser": "INTUOS_V1",
    "Intuos3ReportParser": "INTUOS_V1",
    "Intuos4ReportParser": "INTUOS_V1",
    "IntuosV2ReportParser": "INTUOS_V2",
    "IntuosV3ReportParser": "INTUOS_V3",
    "BambooReportParser": "BAMBOO",
    "GraphireReportParser": "BAMBOO",
}

seen = {}
print("/* pid, name, max x, max y, width um, height um, report format, init reports */")
for path in sorted(
    glob.glob(
        sys.argv[1] + "/OpenTabletDriver.Configurations/Configurations/Wacom/*.json"
    )
):
    with open(path, encoding="utf-8-sig") as f:
        cfg = json.load(f)
        dig = cfg["Specifications"]["Digitizer"]
        for ident in cfg["DigitizerIdentifiers"]:
            parser = ident["ReportParser"].rsplit(".", 1)[-1]
            pid = ident["ProductID"]
            if parser.startswith(
                "WacomDriver"
            ):  # windows with wacom's own driver installed
                continue
            if parser not in FMT:
                print(f"skipped {cfg['Name']} ({pid:04x}): {parser}", file=sys.stderr)
                continue
            row = (
                cfg["Name"],
                dig["MaxX"],
                dig["MaxY"],
                round(dig["Width"] * 1000),
                round(dig["Height"] * 1000),
                FMT[parser],
            )
            if pid in seen:
                if seen[pid] != row:
                    print(
                        f"conflict {pid:04x}: kept {seen[pid]}, dropped {row}",
                        file=sys.stderr,
                    )
                continue
            seen[pid] = row
            init = [base64.b64decode(r) for r in ident.get("FeatureInitReport") or []]
            assert (
                len(init) <= 2
                and all(len(r) == 2 for r in init)
                and not ident.get("OutputInitReport")
            ), path
            inits = ", ".join("{ 0x%02x, 0x%02x }" % tuple(r) for r in init) or "{ 0 }"
            print(
                '\tTABLET(0x%04x, "%s", %d, %d, %d, %d, %s, %d, { %s })'
                % (pid, *row, len(init), inits)
            )
