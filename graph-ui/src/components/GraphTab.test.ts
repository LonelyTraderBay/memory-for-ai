import { describe, expect, it } from "vitest";
import { formatGraphLimitNotice, preserveEnabledFilters } from "./GraphTab";
import type { GraphData } from "../lib/types";

describe("formatGraphLimitNotice", () => {
  it("reports when the graph response is truncated for render safety", () => {
    const data = {
      nodes: Array.from({ length: 2000 }, (_, id) => ({
        id,
        x: 0,
        y: 0,
        z: 0,
        label: "Function",
        name: `fn${id}`,
        size: 1,
        color: "#ffffff",
      })),
      edges: [],
      total_nodes: 43729,
    } satisfies GraphData;

    expect(formatGraphLimitNotice(data)).toBe(
      "Showing 2,000 of 43,729 nodes (0 edges). Raise the node budget or use filters.",
    );
  });

  it("stays quiet when the full graph is rendered", () => {
    const data = {
      nodes: [],
      edges: [],
      total_nodes: 0,
    } satisfies GraphData;

    expect(formatGraphLimitNotice(data)).toBeNull();
  });
});

describe("preserveEnabledFilters", () => {
  const prev = new Set(["Function", "Class"]);

  it("keeps labels the user disabled across a reload", () => {
    const available = new Set(["Function", "Class"]);
    const enabled = new Set(["Class"]); // user disabled "Function"
    expect(preserveEnabledFilters(available, prev, enabled)).toEqual(new Set(["Class"]));
  });

  it("drops labels that vanished and enables brand-new ones", () => {
    const available = new Set(["Function", "Interface"]); // Class gone, Interface new
    const enabled = new Set(["Function", "Class"]);
    expect(preserveEnabledFilters(available, prev, enabled)).toEqual(
      new Set(["Function", "Interface"]),
    );
  });

  it("does not resurrect a label the user disabled that is still present", () => {
    const available = new Set(["Function", "Class"]);
    const enabled = new Set<string>(); // user disabled everything
    expect(preserveEnabledFilters(available, prev, enabled)).toEqual(new Set());
  });
});
