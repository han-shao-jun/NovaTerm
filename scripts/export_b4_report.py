#!/usr/bin/env python3
"""Re-render the self-contained B4 HTML record. Requires Python-Markdown.

The HTML's b4-data JSON block is the sole maintained record. Export a temporary
JSON with --export-data PATH; edit it and re-render with --data PATH. Running
without options rebuilds the layout from the data already embedded in the HTML.
"""
from pathlib import Path
from collections import Counter
import argparse
import html
import json
import re
import tempfile
import markdown

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "docs/architecture/stages/P8_AI_MCP_Interface"
OUTPUT = STAGE / "B4_Test_Report.html"
FAMILIES = {"A":"接入与观察","S":"交互与任务","U":"UART","C":"清空与权限","B":"双通道",
"P":"平台与性能","SF":"SFTP上传","DF":"SFTP下载","UF":"UART文件","OF":"覆盖传输",
"F":"文件边界","H":"历史录制","X":"上传确认","V":"参数与提权"}
STATES = {"失败":"failed","待复验":"retest","部分验证":"partial","未执行":"todo","环境受限":"blocked"}
e = html.escape

def prose(text):
    return markdown.markdown(text, extensions=["extra"]).removeprefix("<p>").removesuffix("</p>")

def checklist_parts(checklist, steps):
    """Present guidance and tool mappings without duplicating the case list."""
    sections = {item["number"]: {"title": item["title"], "lines": item["body"].splitlines()}
                for item in checklist["sections"]}

    def text_of(number):
        return "\n".join(sections[number]["lines"])

    def checklist_items(number):
        return "".join(f'<li>{prose(item)}</li>' for item in re.findall(
            r"^- \[ \] (.+)$", text_of(number), re.M))

    def field_items(number):
        return "".join(f'<li>{prose(field)}</li>' for field in re.findall(
            r"^\| ([^|]+?) \|\s*\|$", text_of(number), re.M))

    tools = [(item["name"], item["positive"], item["negative"]) for item in checklist["tools"]]
    assert len(tools) == 20 and len({row[0] for row in tools}) == 20, "工具映射应有20项"
    by_case = {ident: [] for ident in steps}

    def case_links(value):
        def link(match):
            ident = match[0]
            assert ident in steps, f"工具映射引用未知用例：{ident}"
            return f'<a href="#case-{ident}">{ident}</a>'
        return re.sub(r"\b[A-Z]+\d+\b", link, e(value))

    tool_rows = []
    for tool, positive, negative in tools:
        for ident in set(re.findall(r"\b[A-Z]+\d+\b", positive + "/" + negative)):
            by_case[ident].append(tool)
        tool_rows.append(f'<tr id="tool-{tool}"><th scope="row"><code>novaterm_{tool}</code></th>'
                         f'<td>{case_links(positive)}</td><td>{case_links(negative)}</td></tr>')

    contexts, context_by_case = [], {}
    for number, section in sections.items():
        if not 3 <= int(number.split(".")[0]) <= 10:
            continue
        content = "\n".join(line for line in section["lines"] if not line.startswith("|"))
        if not content.strip():
            continue
        anchor = "guide-" + number.replace(".", "-")
        contexts.append(f'<article class="scenario" id="{anchor}"><h4>{e(section["title"])}</h4>'
                        f'{markdown.markdown(content, extensions=["extra"])}</article>')
        for case in checklist["cases"]:
            if case["section"] == number:
                context_by_case[case["id"]] = anchor
    # The upload and common-boundary tables inherit the file-transfer introduction.
    for ident in steps:
        if ident.startswith(("SF", "F")):
            context_by_case.setdefault(ident, "guide-9")
    instructions = "\n".join(line for line in sections["2"]["lines"]
                             if not line.startswith("|") and not line.startswith("顺序："))
    order = re.search(r"^顺序：(.+)$", text_of("2"), re.M)[1].rstrip("。")
    starts = ["A01", "S01", "U01", "C01", "H01", "X01", "B01", "P01"]
    stages = order.split(" → ")
    assert len(stages) == len(starts)
    order_html = "".join(f'<a href="#case-{ident}"><span>{index}</span>{e(label)}</a>'
                         for index, (ident, label) in enumerate(zip(starts, stages), 1))
    guide = f"""
<div class="checklist-flow" aria-label="建议执行顺序">{order_html}</div>
<div class="guide-grid">
<details class="guide-card" id="checklist-prepare"><summary><strong>执行前准备</strong><span>环境、范围与证据</span></summary>
<div class="guide-content"><ul class="check-items">{checklist_items("1")}</ul>
<h4>每轮先记录这 12 项环境信息</h4><ul class="field-list">{field_items("1")}</ul>
<p class="muted">环境与证据写入本页的本轮结果记录；浏览器内不直接保存填写内容。</p></div></details>
<details class="guide-card" id="checklist-record"><summary><strong>怎样记录结果</strong><span>完整条目与子检查分开</span></summary>
<div class="guide-content">{markdown.markdown(instructions)}<h4>每条记录包含</h4><ul class="field-list">{field_items("2")}</ul></div></details>
<details class="guide-card" id="checklist-tools"><summary><strong>20 工具覆盖映射</strong><span>点用例编号查看步骤与结果</span></summary>
<div class="guide-content"><p>20/20 仅表示有计划用例，不等于执行通过。工具名均带 <code>novaterm_</code> 前缀。</p>
<div class="tool-table-wrap"><table class="tool-table"><thead><tr><th>工具</th><th>正向</th><th>负向 / 恢复</th></tr></thead>
<tbody>{''.join(tool_rows)}</tbody></table></div></div></details>
<details class="guide-card" id="checklist-closeout"><summary><strong>验收收口检查</strong><span>5 项核对，结果统一记账</span></summary>
<div class="guide-content"><ul class="check-items">{checklist_items("12")}</ul>
<a href="#cases">回到用例与当前结果 ↗</a></div></details>
</div><details class="guide-card scenario-guide" id="checklist-scenarios"><summary><strong>各场景的前提与边界</strong><span>Shell、UART、文件传输与隔离设施</span></summary>
<div class="guide-content scenario-grid">{''.join(contexts)}</div></details>
<details class="guide-card scenario-guide" id="checklist-baseline"><summary><strong>清单基线与退役编号</strong><span>版本、平台与当前契约</span></summary>
<div class="guide-content">{markdown.markdown(checklist["introductionMarkdown"], extensions=["extra"])}</div></details>"""
    return guide, by_case, context_by_case

def load_data():
    match = re.search(r'<script type="application/json" id="b4-data">(.*?)</script>',
                      OUTPUT.read_text(), re.S)
    if not match:
        raise ValueError("HTML缺少b4-data主记录，不能从已渲染的页面猜测或覆盖数据")
    return json.loads(match[1])

def build(data):
    assert data["schemaVersion"] == 1, "不支持的数据版本"
    checklist = data["checklist"]
    references = data["results"]["references"]
    steps = {item["id"]: (item["operation"], item["criterion"]) for item in checklist["cases"]}
    titles = {item["id"]: item["title"] for item in checklist["cases"]}
    rows = [tuple(item[field] for field in ("id", "status", "scope", "proof", "gap", "refs"))
            for item in data["results"]["cases"]]
    assert len(rows)==len(steps)==87 and len({r[0] for r in rows})==87
    assert {row[0] for row in rows} == set(steps), "结果条目与清单不一致"
    assert len(checklist["cases"]) == len(steps), "清单用例不得重复"
    guide, tools_by_case, context_by_case = checklist_parts(checklist, steps)
    counts = Counter(row[1] for row in rows)
    case_html=[]
    refs_to_cases={key:[] for key in references}
    for ident,status,scope,proof,gap,refs in rows:
        step, criterion = steps[ident]
        family = re.match(r"[A-Z]+",ident)[0]
        ref_keys=re.findall(r"\[([A-Z][A-Z0-9]*)\]",refs)
        assert all(key in references for key in ref_keys), f"未知执行记录：{ident}"
        for key in ref_keys: refs_to_cases[key].append(ident)
        title=titles[ident]
        links="".join(f'<a class="link-chip" href="#{references[key]["anchor"]}">{key} 执行记录 ↗</a>'
                      for key in ref_keys)
        links+="".join(f'<a class="link-chip quiet" href="#{references[key]["evidenceAnchor"]}">{key} 证据 ↗</a>'
                       for key in ref_keys)
        for issue in data["results"]["issues"]:
            if any(link["target"] == "case-" + ident for link in issue.get("links", [])):
                links+=f'<a class="link-chip warm" href="#{issue["anchor"]}">{e(issue["title"])} ↗</a>'
        if not links: links='<span class="muted">暂无执行记录，按右侧判据补测后登记。</span>'
        guide_links = ''.join(f'<a href="#tool-{tool}"><code>{tool}</code></a>'
                              for tool in tools_by_case[ident])
        if ident in context_by_case:
            guide_links += f'<a href="#{context_by_case[ident]}">本组前提 ↗</a>'
        guide_links += '<a href="#checklist-record">记录口径 ↗</a>'
        search=" ".join([ident,FAMILIES[family],status,scope,step,criterion,proof,gap,*ref_keys]).lower()
        case_html.append(f"""
<details class="case" id="case-{ident}" data-case="{ident}" data-state="{STATES[status]}" data-family="{family}" data-runs="{' '.join(ref_keys)}" data-search="{e(search,quote=True)}">
<summary><span class="case-id">{ident}</span><span class="case-title">{e(title)}<small>{FAMILIES[family]} · {e(scope)}</small></span>
<span class="badge {STATES[status]}">{status}</span><span class="brief">{e(proof)}</span><span class="expand" aria-hidden="true">＋</span></summary>
<div class="case-body"><div class="detail-grid">
<div><h4>测试操作</h4><p>{prose(step)}</p><h4>通过判据</h4><p>{prose(criterion)}</p></div>
<div><h4>已验证到哪里</h4><p>{e(proof)}</p><p class="scope">证据范围：{e(scope)}</p><h4>还需要验证</h4><p>{e(gap)}</p></div>
</div><div class="case-guidance"><span>清单参考</span>{guide_links}</div><div class="case-links">{links}<a class="permalink" href="#case-{ident}" aria-label="{ident}固定链接">#{ident}</a></div></div></details>""")

    cards=[]
    run_defs=data["results"]["runs"]
    for run in run_defs:
        key,anchor,date,title,score,unit,scope,note,raw,cls = (run[field] for field in (
            "key","anchor","date","title","score","unit","scope","note","detailAnchor","className"))
        case_links="".join(f'<a href="#case-{ident}">{ident}</a>' for ident in refs_to_cases[key])
        cards.append(f"""<article class="run-card {cls}" id="{anchor}">
<div class="run-top"><span class="mono">{key}</span><time>{date}</time></div><h3>{title}</h3>
<div class="score">{score}<small>{unit}</small></div><p class="run-scope">{scope}</p><p class="muted">{note}</p>
<div class="run-links"><a href="#{raw}">详细记录 ↗</a><a href="#{references[key]["evidenceAnchor"]}">证据 ↗</a></div>
<details class="related"><summary>关联用例 · {len(refs_to_cases[key])}</summary><div class="case-chips">{case_links}</div></details></article>""")
    evidence_items=data["results"]["evidence"]
    evidence_html="".join(f'<article class="evidence-item" id="{item["anchor"]}"><span class="mono">{e(item["key"])}</span><div><h3>{e(item["title"])}</h3><code>{e(item["path"])}</code><p>{e(item["note"])}</p></div></article>'
                          for item in evidence_items)
    issue_cards = []
    for issue in data["results"]["issues"]:
        metrics = ''.join(f'<div><strong>{e(item["value"])}</strong><span>{e(item["label"])}</span></div>' for item in issue["stats"])
        stats_id = f' id="{issue["statsAnchor"]}"' if issue.get("statsAnchor") else ''
        issue_links = ''.join(f'<a href="#{item["target"]}">{e(item["label"])} ↗</a>' for item in issue["links"])
        issue_cards.append(f'<article class="fix" id="{issue["anchor"]}"><div class="fix-header"><h3>{e(issue["title"])}</h3>'
                           f'<span class="badge {issue["state"]}">{e(issue["status"])}</span></div><p>{e(issue["description"])}</p>'
                           f'<div class="result-list"{stats_id}>{metrics}</div><p class="{issue["noteClass"]}">{e(issue["note"])}</p>'
                           f'<div class="fix-links">{issue_links}</div></article>')
    overview = data["overview"]
    priority = overview["priority"]
    flow = '<i></i>'.join(f'<b>{e(step)}</b>' if index < len(priority["steps"])-1 else f'<span class="next">{e(step)}</span>'
                         for index,step in enumerate(priority["steps"]))
    priority_html = f'<div class="priority-top"><span>{e(priority["label"])}</span><a href="#{priority["target"]}">查看问题 ↗</a></div>' + \
                    f'<h2>{e(priority["title"])}</h2><p>{e(priority["summary"])}</p><div class="flow">{flow}</div>'
    # Only the opt-in provenance area is rendered from Markdown.
    archive_source=data["results"]["historyMarkdown"]
    archive_source=archive_source.replace('<a id="archives"></a>',"").replace("## 7. 历史原文","")
    archive_source=archive_source.replace("<details>",'<details markdown="1">')
    archive_source=archive_source.replace('id="b4-01-code-fix"','id="fix-detail"').replace('id="font-fix"','id="font-detail"')
    archive=markdown.markdown(archive_source,extensions=["extra"])
    archive += '<details><summary>迁入 HTML 时的摘要与判据审查记录（历史快照）</summary>' + markdown.markdown(
        data["results"]["migrationSnapshotMarkdown"], extensions=["extra"]) + '</details>'
    # Fix relative links from records/ to this file's directory.
    archive=archive.replace('href="../B4_Test_Report.html','href="B4_Test_Report.html')
    archive=re.sub(r'href="#result-([A-Z]+\d+)"', r'href="#case-\1"', archive)
    archive=archive.replace('href="#case-results"', 'href="#cases"')
    archive=archive.replace('href="#font-fix"', 'href="#font-detail"')
    stats='<button class="stat selected" data-state-filter=""><strong>87</strong><span>全部用例</span></button>'
    for label,key in STATES.items():
        if not counts[label]:
            continue
        stats+=f'<button class="stat {key}" data-state-filter="{key}"><strong>{counts[label]}</strong><span>{label}</span></button>'
    family_options='<option value="">全部类别</option>'+''.join(f'<option value="{k}">{k} · {v}</option>' for k,v in FAMILIES.items())
    state_options='<option value="">全部状态</option>'+''.join(f'<option value="{v}">{k}</option>' for k,v in STATES.items())
    run_options='<option value="">全部记录</option>'+''.join(f'<option value="{key}">{key} · {e(ref["label"])}</option>' for key,ref in references.items())
    page=TEMPLATE
    for token,value in {
        "__CSS__":CSS,"__JS__":JS,"__STATS__":stats,"__CASES__":"".join(case_html),
        "__FAMILIES__":family_options,"__STATES__":state_options,"__RUNS_FILTER__":run_options,
        "__RUNS__":"".join(cards),"__EVIDENCE__":evidence_html,"__ARCHIVES__":archive,"__CHECKLIST__":guide,
        "__ISSUES__":"".join(issue_cards),"__PRIORITY__":priority_html,
        "__OVERALL__":e(overview["status"]),"__VERSION__":e(overview["version"]),
        "__UPDATED__":e(overview["updated"]),"__SUMMARY__":e(overview["summary"]),
        "__DATA__":json.dumps(data, ensure_ascii=False, indent=2).replace("<", "\\u003c"),
    }.items():page=page.replace(token,value)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=OUTPUT.parent, suffix=".tmp", delete=False) as pending:
        pending.write(page)
    Path(pending.name).replace(OUTPUT)
    print(f"{OUTPUT}\n87 case records; states={dict(counts)}; {OUTPUT.stat().st_size} bytes")

CSS=r"""
:root{--bg:#f4f5f7;--paper:#fff;--ink:#1f2c3c;--muted:#788391;--line:#e5e8ed;--teal:#167b70;--blue:#4678b6;--amber:#a76818;--rose:#af4f45}
*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:94px}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.75 system-ui,-apple-system,"Segoe UI","Noto Sans CJK SC","Microsoft YaHei",sans-serif}button,input,select{font:inherit}a{color:var(--teal);text-decoration:none}a:hover{text-decoration:underline;text-underline-offset:4px}button,summary{cursor:pointer}button:focus-visible,a:focus-visible,summary:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #8ccfc3;outline-offset:3px}[hidden]{display:none!important}
.topbar{height:66px;background:#fff;border-bottom:1px solid var(--line);position:sticky;top:0;z-index:5}.top-inner{max-width:1320px;height:100%;margin:auto;display:flex;align-items:center;gap:30px;padding:0 32px}.brand{display:flex;align-items:center;gap:11px;font-weight:750;font-size:17px;letter-spacing:-.5px}.brand-icon{background:#173e43;color:#b2e9d9;border-radius:8px;width:31px;height:31px;display:grid;place-items:center;font:17px monospace}.brand small{border-left:1px solid var(--line);padding-left:14px;font-weight:400;color:var(--muted);font-size:13px}.topbar nav{margin-left:auto;display:flex;gap:24px;font-size:13px}.topbar nav a{color:#596779}.print-btn{border:1px solid var(--line);background:white;border-radius:6px;padding:6px 12px;font-size:12px;color:#596779}
main{max-width:1320px;margin:auto;padding:38px 32px 80px}.hero{display:grid;grid-template-columns:1.05fr 1fr;gap:40px;align-items:center;margin-bottom:30px}.eyebrow{font-size:11px;letter-spacing:1.8px;font-weight:650;color:var(--teal);margin:0 0 10px}h1{font-size:34px;line-height:1.4;letter-spacing:-.9px;margin:0 0 10px}.hero p{color:var(--muted);font-size:13px;margin:9px 0}.overall{display:inline-flex;align-items:center;gap:6px;background:#fff3df;color:#8c5d1b;padding:3px 9px;border-radius:5px;font-size:12px;font-weight:600;margin-right:8px}.overall:before{content:"";height:6px;width:6px;background:#be893d;border-radius:50%}
.priority{background:var(--paper);border:1px solid #dfcba8;border-left:3px solid #bd8e41;border-radius:9px;padding:19px 22px;box-shadow:0 5px 16px #29384c03}.priority-top{display:flex;align-items:center;justify-content:space-between;font-size:12px;color:var(--amber)}.priority h2{font-size:17px;margin:9px 0 6px}.priority p{font-size:12px;margin:0 0 13px;color:#7c725f}.flow{display:flex;align-items:center;gap:10px;font-size:11px;color:var(--muted)}.flow b{color:var(--teal);font-weight:500}.flow .next{color:var(--amber)}.flow i{height:1px;flex:1;background:#e2e7e7}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:13px}.stat{border:1px solid var(--line);border-radius:8px;background:var(--paper);text-align:left;padding:17px 20px;display:flex;align-items:center;gap:14px;color:#647183;transition:border-color .15s,transform .15s}.stat:hover{border-color:#98bcb4;transform:translateY(-1px)}.stat.selected{border-color:var(--teal);box-shadow:inset 0 0 0 1px var(--teal)}.stat strong{font-size:28px;line-height:1.1;font-weight:600;letter-spacing:-.5px;color:var(--ink);font-variant-numeric:tabular-nums}.stat span{font-size:12px}.stat.failed strong{color:var(--rose)}.stat.retest strong{color:var(--amber)}.stat.partial strong{color:var(--teal)}.stat.blocked strong{color:#8b637a}.count-note{color:var(--muted);font-size:11px;margin:0 0 32px}
.section-head{display:flex;justify-content:space-between;align-items:center;gap:20px;margin-bottom:17px}.section-head h2{font-size:20px;margin:0;letter-spacing:-.3px}.section-head p{color:var(--muted);font-size:12px;margin:3px 0 0}.section-head .number{font:11px ui-monospace,monospace;color:#9ba5b1;margin-right:8px}.text-link{font-size:12px;white-space:nowrap}
.checklist-section{margin-bottom:32px}.checklist-flow{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}.checklist-flow a{display:flex;align-items:center;gap:7px;font-size:11px;border:1px solid var(--line);background:white;padding:7px 10px;border-radius:5px}.checklist-flow span{font:11px ui-monospace,monospace;color:var(--muted)}.guide-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;align-items:start}.guide-card{border:1px solid var(--line);background:white;border-radius:8px;min-width:0}.guide-card>summary{padding:14px 18px;font-size:12px;color:#4e6e65}.guide-card>summary strong{font-weight:600}.guide-card>summary span{color:var(--muted);font-size:11px;margin-left:12px}.guide-content{padding:0 20px 17px;font-size:12px;color:#536575;overflow-wrap:anywhere}.guide-content p{margin:10px 0}.guide-content h4{font-size:12px;color:#45685e;margin:14px 0 8px}.guide-content ul{padding-left:20px}.guide-content li{margin-bottom:8px}.check-items{list-style:'□  '}.field-list{columns:2;column-gap:26px}.field-list li{break-inside:avoid}.scenario-guide{margin-top:10px}.scenario-grid{display:grid;grid-template-columns:1fr 1fr;gap:0 30px}.scenario{border-top:1px solid var(--line);padding-top:6px;scroll-margin-top:90px}.tool-table-wrap{overflow:auto}.tool-table{border-collapse:collapse;width:100%;font-size:11px}.tool-table th,.tool-table td{text-align:left;vertical-align:top;padding:9px 8px;border-bottom:1px solid var(--line)}.tool-table th{font-weight:500}.tool-table thead{color:var(--muted)}.tool-table code{font-size:10px;overflow-wrap:anywhere}.tool-table a{display:inline-block;padding:1px 2px}.tool-table tr{scroll-margin-top:90px}.case-guidance{display:flex;gap:8px 13px;align-items:center;flex-wrap:wrap;font-size:11px;margin:0 0 16px}.case-guidance span{color:var(--muted)}.case-guidance code{font-size:10px}
.case-panel{border:1px solid var(--line);background:white;border-radius:10px;overflow:hidden}.filters{padding:18px 20px 13px;display:grid;grid-template-columns:minmax(180px,1fr) repeat(3,150px) auto;gap:10px;align-items:end;background:#fff}.filters label{font-size:11px;color:var(--muted);display:grid;gap:6px}.filters input,.filters select{width:100%;border:1px solid #dce2e9;background:#fafbfc;color:var(--ink);border-radius:6px;padding:9px 10px;font-size:12px;min-height:39px}.reset{background:white;border:0;color:var(--teal);font-size:12px;padding:10px 5px}.filter-meta{padding:0 20px 13px;display:flex;justify-content:space-between;font-size:11px;color:var(--muted);border-bottom:1px solid var(--line)}.filter-meta button{border:0;background:none;color:var(--muted);font-size:11px}.columns{display:grid;grid-template-columns:66px minmax(180px,1fr) 96px minmax(160px,1fr) 18px;gap:18px;padding:10px 20px;background:#f6f8fa;color:#8b95a2;font-size:11px}
.case{border-top:1px solid #edf0f3;scroll-margin-top:82px}.case:first-of-type{border-top:0}.case>summary{list-style:none;display:grid;grid-template-columns:66px minmax(180px,1fr) 96px minmax(160px,1fr) 18px;gap:18px;padding:16px 20px;align-items:center;transition:background .1s}.case>summary::-webkit-details-marker{display:none}.case>summary:hover{background:#fafcfb}.case[open]>summary{background:#eff7f4;border-bottom:1px solid #dcebe5}.case-id{font:600 12px ui-monospace,monospace;color:#355d62;letter-spacing:.2px}.case-title{font-weight:550;font-size:13px;line-height:1.6}.case-title small{display:block;font-weight:400;font-size:11px;color:var(--muted);margin-top:5px}.brief{font-size:12px;color:#718091;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.badge{border-radius:4px;font-size:11px;padding:3px 7px;width:max-content;border:1px solid transparent;white-space:nowrap}.badge.failed{background:#fff0ed;color:#a44236}.badge.partial{background:#e8f4ef;color:#277e63}.badge.retest{background:#fff2dd;color:#a37020}.badge.todo{background:#f0f2f5;color:#7a8593}.badge.blocked{background:#f4edf3;color:#8c6085}.expand{color:#8ca29b;font-size:18px}.case[open] .expand{font-size:0}.case[open] .expand:after{content:"−";font-size:18px}.case-body{padding:23px 24px 17px 104px}.detail-grid{display:grid;grid-template-columns:1fr 1fr;gap:36px}.detail-grid h4{font-size:11px;font-weight:600;letter-spacing:.4px;color:#55736b;margin:0 0 8px}.detail-grid p{font-size:13px;margin:0 0 18px;overflow-wrap:anywhere;color:#415363}.detail-grid .scope{font-size:11px;color:var(--muted)}.case-links{display:flex;gap:8px;align-items:center;flex-wrap:wrap;border-top:1px solid var(--line);padding-top:14px}.link-chip{font-size:11px;padding:4px 8px;border:1px solid #cddfd8;border-radius:4px;background:#fbfdfc}.link-chip.quiet{border-color:#e5e8ed;color:var(--muted)}.link-chip.warm{border-color:#e2d1ae;color:var(--amber)}.permalink{margin-left:auto;font:11px monospace;color:#8b99a5}.muted{color:var(--muted)}#empty{padding:40px;text-align:center;color:var(--muted)}#empty strong{display:block;font-size:16px;margin-bottom:8px}
.below{margin-top:42px}.run-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}.run-card{background:white;border:1px solid var(--line);border-radius:9px;padding:20px;scroll-margin-top:86px}.run-card.has-failure{border-top:3px solid #bb7664;padding-top:18px}.run-top{display:flex;justify-content:space-between;font-size:11px;color:var(--muted)}.mono{font-family:ui-monospace,monospace;font-size:12px;color:#426278}.run-card h3{font-size:16px;margin:12px 0 19px}.score{font-size:28px;font-weight:550;letter-spacing:-.8px;line-height:1.3}.score small{font-size:10px;font-weight:400;color:var(--muted);display:block;letter-spacing:0;margin-top:3px}.run-card p{font-size:12px;line-height:1.8}.run-scope{color:#455d6b;margin-top:20px}.run-links{display:flex;gap:16px;padding:13px 0;border-top:1px solid var(--line);font-size:11px}.related>summary{font-size:11px;color:var(--muted)}.case-chips{display:flex;flex-wrap:wrap;gap:5px;margin-top:10px}.case-chips a{font:11px monospace;background:#eef4f4;padding:4px 6px;border-radius:3px}
.fix-grid{display:grid;grid-template-columns:1.6fr 1fr;gap:16px}.fix{background:white;border:1px solid var(--line);border-radius:9px;padding:24px}.fix-header{display:flex;align-items:center;gap:12px;margin-bottom:12px}.fix h3{font-size:16px;margin:0}.fix p{font-size:13px;color:#647384}.fix .result-list{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;padding:15px 0;border-top:1px solid var(--line);border-bottom:1px solid var(--line);margin:16px 0}.result-list strong{display:block;font-size:20px;color:var(--teal);font-weight:550}.result-list span{font-size:10px;color:var(--muted)}.next-step{padding:10px 13px;background:#fffaef;border-radius:5px;color:#8b6a32!important;font-size:12px!important}.fix-links{display:flex;gap:18px;flex-wrap:wrap;font-size:12px}.evidence-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.evidence-item{display:flex;gap:17px;padding:18px;background:white;border:1px solid var(--line);border-radius:7px;scroll-margin-top:85px;min-width:0}.evidence-item>div{min-width:0}.evidence-item h3{font-size:13px;font-weight:550;margin:0 0 7px}.evidence-item code{font:11px/1.7 ui-monospace,monospace;color:#627380;overflow-wrap:anywhere}.evidence-item p{color:var(--muted);font-size:11px;margin:7px 0 0;overflow-wrap:anywhere}
.archive-box{background:white;border:1px solid var(--line);border-radius:9px;padding:20px 24px}.archive-box details{margin:14px 0;border-top:1px solid var(--line);padding-top:14px}.archive-box summary{font-size:13px;color:#54756d}.archive-box h1{font-size:20px;margin-top:22px}.archive-box h2{font-size:17px;margin-top:28px}.archive-box h3{font-size:15px;margin-top:22px}.archive-box p,.archive-box li{font-size:13px;overflow-wrap:anywhere}.archive-box table{font-size:12px;width:100%;border-collapse:collapse;display:block;overflow:auto}.archive-box td,.archive-box th{padding:9px;border:1px solid var(--line);vertical-align:top;min-width:100px}.archive-box th{background:#f5f7fa}.archive-box code{font-size:11px;overflow-wrap:anywhere}.archive-box pre{white-space:pre-wrap;background:#f5f7fa;padding:15px}.anchor-flash{animation:flash 1.5s ease-out}@keyframes flash{from{box-shadow:0 0 0 3px #aacfc0}to{box-shadow:none}}footer{font-size:11px;color:var(--muted);margin-top:26px;display:flex;justify-content:space-between;gap:20px}.skip{position:fixed;top:-80px}.skip:focus{top:6px;left:10px;z-index:10;background:white;padding:10px}
@media(max-width:1050px){.hero{gap:20px}.filters{grid-template-columns:1fr 140px 140px}.filters label:first-child{grid-column:span 2}.case>summary,.columns{grid-template-columns:54px 1fr 90px 20px;gap:12px}.brief,.columns .brief-head{display:none}.case-body{padding-left:30px}.run-grid{grid-template-columns:1fr 1fr}.stat{padding:14px;gap:10px}.stat strong{font-size:23px}.topbar nav{gap:14px}.brand small{display:none}}
@media(max-width:680px){.top-inner{padding:0 16px;gap:12px}.topbar{height:58px}.topbar nav a:nth-child(n+3){display:none}.topbar nav{gap:12px}.brand{font-size:14px}.print-btn{display:none}main{padding:25px 15px 40px}.hero{grid-template-columns:1fr;gap:15px;margin-bottom:22px}h1{font-size:28px}.priority{padding:16px 18px}.stats{display:flex;overflow:auto;gap:8px;padding-bottom:4px}.stat{min-width:118px;flex-direction:column;align-items:flex-start;gap:5px;padding:13px}.count-note{font-size:10px;margin-bottom:24px}.section-head p{font-size:11px}.filters{padding:15px;grid-template-columns:1fr 1fr;gap:8px}.filters label:first-child{grid-column:span 2}.filters label:nth-child(4){grid-column:1}.columns{display:none}.case>summary{padding:14px 15px;grid-template-columns:42px 1fr auto;gap:10px}.case-title{font-size:12px}.case-title small{font-size:10px}.expand{display:none}.badge{font-size:10px}.case-body{padding:18px}.detail-grid{grid-template-columns:1fr;gap:0}.case-links{gap:6px}.run-grid{grid-template-columns:1fr 1fr;gap:10px}.run-card{padding:16px}.score{font-size:24px}.fix-grid,.evidence-grid{grid-template-columns:1fr}.fix{padding:20px}.archive-box{padding:16px}.section-head h2{font-size:18px}.text-link{font-size:11px}.filter-meta{padding:0 15px 12px}.below{margin-top:30px}footer{display:block}footer a{display:inline-block;margin-top:8px}}
@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}*{animation:none!important;transition:none!important}}
@media(max-width:680px){.guide-grid,.scenario-grid{grid-template-columns:1fr}.guide-card>summary span{display:block;margin-left:15px}.field-list{columns:1}.checklist-flow{gap:6px}.checklist-flow a{font-size:10px}.tool-table th,.tool-table td{padding:8px 5px}}
@media print{body{background:white;font-size:10pt}.topbar,.filters,.stats,.count-note,.filter-meta,.columns,.skip,.priority,.reset,.print-btn{display:none!important}main{padding:0}.hero{display:block}.case[hidden]{display:block!important}.case>summary{display:flex;gap:15px}.case-body{padding:15px}.detail-grid{display:block}.brief{display:none}.case,.run-card,.fix,.evidence-item{break-inside:avoid}.run-grid,.fix-grid,.evidence-grid{display:block}.run-card,.fix{margin-bottom:15px}.case-panel{overflow:visible}.badge{border:1px solid #aaa}footer{display:none}.archive-box table{display:table}h2,h3,summary{break-after:avoid}}
"""
JS=r"""
(() => {
 const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
 const cases=$$('[data-case]'), controls=['query','state','family','run'];
 const fields=controls.map(id=>document.getElementById('filter-'+id));
 function apply(){
  const [query,state,family,run]=fields.map(el=>el.value.trim());
  const words=query.toLocaleLowerCase().split(/\s+/).filter(Boolean);
  let count=0;
  cases.forEach(el=>{
   el.hidden=!!((state&&el.dataset.state!==state)||(family&&el.dataset.family!==family)||
    (run&&!el.dataset.runs.split(' ').includes(run))||words.some(w=>!el.dataset.search.includes(w)));
   if(!el.hidden)count++;
  });
  $('#visible-count').textContent=count+' / '+cases.length+' 条用例';
  $('#empty').hidden=count!==0;
  $$('[data-state-filter]').forEach(b=>b.classList.toggle('selected',b.dataset.stateFilter===state));
  $('#filter-description').textContent=fields.some(el=>el.value.trim())?'筛选已生效 · 点击用例展开结果与证据':'完整判据未全部验证时，不标为整项通过';
 }
 function reset(){fields.forEach(el=>el.value='');apply();}
 fields.forEach(el=>el.addEventListener(el.tagName==='SELECT'?'change':'input',apply));
 $('#reset-filters').addEventListener('click',reset);
 $('#empty-reset').addEventListener('click',reset);
 $$('[data-state-filter]').forEach(button=>button.addEventListener('click',()=>{
  fields[1].value=button.dataset.stateFilter;apply();$('#cases').scrollIntoView({block:'start'});
 }));
 $('#collapse-cases').addEventListener('click',()=>cases.forEach(el=>el.open=false));
 function jump(){
  let id;try{id=decodeURIComponent(location.hash.slice(1));}catch{return;}
  const aliases={results:'overview','case-results':'cases','font-fix':'font-detail','result-A04':'case-A04'};
  if(id.startsWith('result-'))id='case-'+id.slice(7);
  id=aliases[id]||id;
  const target=document.getElementById(id);if(!target)return;
  const row=target.closest('[data-case]');
  if(row){if(row.hidden)reset();row.open=true;}
  let el=target;while(el){if(el.tagName==='DETAILS')el.open=true;el=el.parentElement;}
  if(target.nextElementSibling?.tagName==='DETAILS')target.nextElementSibling.open=true;
  target.scrollIntoView({block:'start',behavior:'auto'});
  target.classList.remove('anchor-flash');void target.offsetWidth;target.classList.add('anchor-flash');
 }
 document.addEventListener('click',event=>{
  const link=event.target.closest('a[href^="#"]');if(!link)return;
  event.preventDefault();const hash=link.getAttribute('href');
  if(location.hash===hash)jump();else location.hash=hash;
 });
 window.addEventListener('hashchange',jump);
 $('#print').addEventListener('click',()=>window.print());
 let saved=[];
 window.addEventListener('beforeprint',()=>{saved=$$('details').map(el=>[el,el.open]);saved.forEach(([el])=>el.open=true);});
 window.addEventListener('afterprint',()=>saved.forEach(([el,open])=>el.open=open));
 apply();if(location.hash)jump();
})();
"""
TEMPLATE=r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NovaTerm · B4 测试工作台</title><style>__CSS__</style></head>
<body><a class="skip" href="#cases">跳到用例</a>
<header class="topbar"><div class="top-inner"><a class="brand" href="#overview"><span class="brand-icon">&gt;_</span>NovaTerm <small>测试工作台</small></a>
<nav aria-label="主要导航"><a href="#cases">用例记录</a><a href="#checklist">测试清单</a><a href="#runs">执行轮次</a><a href="#issues">问题修复</a><a href="#evidence">证据归档</a></nav><button class="print-btn" id="print">打印报告</button></div></header>
<main id="overview">
<section class="hero"><div><p class="eyebrow">B4 / AI MCP INTERFACE</p><h1>每条用例，都能追到结果。</h1>
<p><span class="overall">__OVERALL__</span>__VERSION__ · 更新于 __UPDATED__</p>
<p>__SUMMARY__</p></div><aside class="priority">__PRIORITY__</aside></section>
<div class="stats" aria-label="按当前状态筛选">__STATS__</div>
<p class="count-note">以上按 87 条有效用例的完整判据归类，不是子检查累计通过率；10 个退役编号另存历史记录。</p>
<section class="checklist-section" id="checklist"><div class="section-head"><div><h2>测试清单</h2><p>先核对环境与范围，按顺序进入用例；每条操作、判据和当前结果在同一处。</p></div><a class="text-link" href="#cases">查看全部 87 条 ↗</a></div>__CHECKLIST__</section>
<section id="cases"><div class="section-head"><div><h2><span class="number">01</span>用例与当前结果</h2><p>先筛选，再展开；每条记录都保留已测范围与剩余判据。</p></div><a class="text-link" href="#runs">按执行轮次查看 ↗</a></div>
<div class="case-panel"><div class="filters">
<label>查找用例<input id="filter-query" type="search" placeholder="编号、关键词，如 A14 / 中文 / 上传" autocomplete="off"></label>
<label>状态<select id="filter-state">__STATES__</select></label><label>类别<select id="filter-family">__FAMILIES__</select></label>
<label>执行记录<select id="filter-run">__RUNS_FILTER__</select></label><button class="reset" id="reset-filters">重置</button></div>
<div class="filter-meta"><span><b id="visible-count" role="status" aria-live="polite"></b>　<span id="filter-description"></span></span><button id="collapse-cases">收起详情</button></div>
<div class="columns"><span>编号</span><span>测试内容 / 证据范围</span><span>当前状态</span><span class="brief-head">已验证摘要</span><span></span></div>
<noscript><p>浏览器未启用脚本，仍可逐项展开阅读；组合筛选需要启用 JavaScript。</p></noscript>
__CASES__<div id="empty" hidden><strong>没有匹配的用例</strong><p>试试减少关键词或放宽筛选条件。</p><button class="reset" id="empty-reset">重置筛选</button></div></div></section>
<section class="below" id="runs"><div class="section-head"><div><h2><span class="number">02</span>执行轮次</h2><p>每轮独立记账，保留失败当时的事实。</p></div></div><div class="run-grid">__RUNS__</div></section>
<section class="below" id="issues"><div class="section-head"><div><h2><span class="number">03</span>问题与修复</h2><p>把“发现问题”“代码修好”和“实机通过”分开看。</p></div></div>
<div class="fix-grid">__ISSUES__</div></section>
<section class="below" id="evidence"><div class="section-head"><div><h2><span class="number">04</span>证据位置</h2><p>本机临时文件，清理或迁移后可能不可用；结论与边界保留在记录中。</p></div></div><div class="evidence-grid">__EVIDENCE__</div></section>
<section class="below" id="archives"><div class="section-head"><div><h2><span class="number">05</span>详细记录与历史原文</h2><p>需要核对原始参数和过程时再展开；过时判据只用于追溯。</p></div></div><div class="archive-box">__ARCHIVES__</div></section>
<footer><span>本地离线查看 · 本页统一维护清单、结果与历史证据</span><a href="#overview">回到顶部 ↑</a></footer>
</main><script>__JS__</script><script type="application/json" id="b4-data">__DATA__</script></body></html>
"""
if __name__=="__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--export-data", type=Path, help="导出HTML内数据，供临时编辑")
    mode.add_argument("--data", type=Path, help="从编辑后的JSON更新HTML主记录")
    args = parser.parse_args()
    if args.export_data:
        args.export_data.write_text(json.dumps(load_data(), ensure_ascii=False, indent=2) + "\n")
        print(args.export_data)
    else:
        build(json.loads(args.data.read_text()) if args.data else load_data())
