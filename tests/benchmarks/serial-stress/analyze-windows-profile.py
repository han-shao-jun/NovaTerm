"""解析 xperf 的函数统计与线程 HTML；只输出指定压力进程的数据。"""
import argparse
import csv
import json
import re
from html.parser import HTMLParser
from pathlib import Path


class TableRows(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self.row = []
        self.cell = None

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th"):
            self.cell = []
        elif tag == "br" and self.cell is not None:
            self.cell.append(" ")
        elif self.cell is not None and tag not in {"a", "p", "ol", "li", "span", "b", "u", "em"}:
            # xperf 的 HTML 有时不转义 C++ 模板尖括号，保留原始函数签名。
            self.cell.append(self.get_starttag_text())

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None:
            self.row.append("".join(self.cell).replace("\xa0", " ").strip())
            self.cell = None
        elif tag == "tr" and self.row:
            self.rows.append(self.row)


def table(text, section):
    match = re.search(r"<a id='" + section + r"'>.*?(?=<a id='Tbl|\Z)", text, re.S)
    if not match:
        raise ValueError(f"Missing xperf section: {section}")
    parser = TableRows()
    parser.feed(match.group(0))
    return parser.rows


def thread_stats(path, pid):
    text = path.read_text(encoding="utf-8-sig")
    process_rows = table(text, "TblP")
    total = next(int(row[2]) for row in process_rows
                 if len(row) == 4 and row[0] == "serial_stress.exe" and row[1] == str(pid))
    exclusive = []
    for row in table(text, "TblSE"):
        if len(row) != 7 or not row[1].isdigit():
            continue
        exclusive.append({"function": row[0], "hits": int(row[1]),
                          "percent": float(row[2].rstrip("%")), "inclusive_hits": int(row[3])})
    inclusive = []
    for row in table(text, "TblSI"):
        if len(row) != 7 or not row[1].isdigit():
            continue
        inclusive.append({"function": row[0], "hits": int(row[1]),
                          "percent": float(row[2].rstrip("%")), "exclusive_hits": int(row[3])})
    return {"total_hits": total, "exclusive": exclusive, "inclusive": inclusive}


def process_stats(path, pid):
    pattern = re.compile(r"^\s*serial_stress[^,]*\(\s*" + str(pid)
                         + r"\),\s*(\d+),\s*[\d.]+,\s*(.+)$")
    rows = []
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "gb18030", "cp1252"):
        try:
            text = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            rows.append({"function": match[2].strip(), "weight": int(match[1])})
    total = sum(row["weight"] for row in rows)
    if not total:
        raise ValueError(f"No process samples for PID {pid}")
    for row in rows:
        row["percent"] = row["weight"] * 100 / total
    return {"total_weight": total, "functions": sorted(rows, key=lambda row: row["weight"], reverse=True)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    summary = {}
    for mode in ("visible", "hidden"):
        captures = sorted(args.capture_dir.glob(f"{mode}-*.json"))
        if not captures:
            raise ValueError(f"No {mode} capture metadata")
        metadata = json.loads(captures[-1].read_text(encoding="utf-8-sig"))
        if not metadata["pass"] or metadata["missingFrames"]:
            raise ValueError("Profiled workload failed integrity")
        result = {"metadata": metadata,
                  "process": process_stats(args.capture_dir / f"{mode}-profile.csv", metadata["pid"]),
                  "threads": {}}
        for role in ("gui", "parser", "sender"):
            result["threads"][role] = thread_stats(args.capture_dir / f"{mode}-{role}-stack.html", metadata["pid"])
        summary[mode] = result
        for role, stats in result["threads"].items():
            print(f"{mode} {role}: {stats['total_hits']} CPU stack samples")
            for row in stats["exclusive"][:8]:
                print(f"  {row['percent']:6.2f}% {row['function']}")
        output_csv = args.output.with_name(args.output.stem + f"-{mode}-functions.csv")
        with output_csv.open("w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.DictWriter(stream, fieldnames=("function", "weight", "percent"))
            writer.writeheader()
            writer.writerows(result["process"]["functions"])
    args.output.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
