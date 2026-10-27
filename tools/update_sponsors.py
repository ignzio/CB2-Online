#!/usr/bin/env python3
"""Reads the maintainer's GitHub Sponsors totals through the GraphQL API and writes docs/sponsors.json, which the project page reads to fill the milestone bars.

Run by .github/workflows/sponsors.yml every few hours; it can also be run by hand:
    SPONSORS_TOKEN=<token> python tools/update_sponsors.py [output.json]
The token must be allowed to read the maintainer's sponsorships: a *classic* personal access token with the `read:user` scope, created by the sponsored account (the default Actions token
and fine-grained tokens cannot read Sponsors). Nothing about individual sponsors is written: only the totals, so private sponsors stay private.

"raised" = every one-time sponsorship ever made + the monthly pledges that are active now (GitHub gives no history of cancelled monthly payments). Amounts are in US dollars.
"""
import json, os, sys, urllib.request, datetime

QUERY = """
query($after: String) {
  viewer {
    login
    sponsorshipsAsMaintainer(first: 100, after: $after, includePrivate: true, activeOnly: false) {
      pageInfo { hasNextPage endCursor }
      nodes { isActive isOneTimePayment tier { monthlyPriceInCents } }
    }
  }
}
"""

def graphql(token, variables):
    req = urllib.request.Request("https://api.github.com/graphql", data=json.dumps({"query": QUERY, "variables": variables}).encode(),
                                 headers={"Authorization": "Bearer " + token, "User-Agent": "cb2-sponsors", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        body = json.load(r)
    if body.get("errors"):
        raise SystemExit("GitHub answered with errors: " + "; ".join(e.get("message", "?") for e in body["errors"]))
    return body["data"]["viewer"]

def main():
    token = os.environ.get("SPONSORS_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        raise SystemExit("set SPONSORS_TOKEN (a classic personal access token with read:user)")
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "sponsors.json")
    one_time_cents = monthly_cents = monthly_count = one_time_count = 0
    after = None
    while True:
        viewer = graphql(token, {"after": after})
        page = viewer["sponsorshipsAsMaintainer"]
        for n in page["nodes"]:
            cents = (n.get("tier") or {}).get("monthlyPriceInCents") or 0
            if n["isOneTimePayment"]:
                one_time_cents += cents
                one_time_count += 1
            elif n["isActive"]:
                monthly_cents += cents
                monthly_count += 1
        if not page["pageInfo"]["hasNextPage"]:
            break
        after = page["pageInfo"]["endCursor"]
    data = {
        "raised": round((one_time_cents + monthly_cents) / 100, 2),
        "oneTime": round(one_time_cents / 100, 2),
        "monthly": round(monthly_cents / 100, 2),
        "sponsors": monthly_count + one_time_count,
        "currency": "USD",
        "updated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    old = None
    try:
        old = json.load(open(out_path))
    except Exception:
        pass
    if old and all(old.get(k) == data[k] for k in ("raised", "oneTime", "monthly", "sponsors")):
        print("unchanged:", json.dumps(data))
        return
    with open(out_path, "w", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    print("written:", json.dumps(data))

if __name__ == "__main__":
    main()
