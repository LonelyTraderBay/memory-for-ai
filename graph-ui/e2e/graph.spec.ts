import { expect, test } from "@playwright/test";

test("WebGL survives reload, refresh and project switches", async ({ page, context }) => {
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));
  const layouts: string[] = [];
  await context.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === "/api/layout") {
      const project = url.searchParams.get("project")!;
      layouts.push(project);
      const count = project === "alpha" ? 2 : 3;
      return route.fulfill({ json: {
        nodes: Array.from({ length: count }, (_, i) => ({
          id: i + 1, x: i * 20, y: i * 10, z: 0, size: 3, color: "#00aaff",
          label: "Function", name: `${project}_${i}`, qualified_name: `${project}.${i}`,
          file_path: "src/main.c", status: "normal",
        })),
        edges: [{ source: 1, target: 2, type: "CALLS" }], total_nodes: count,
      } });
    }
    return route.fulfill({ json: {} });
  });
  await context.route("**/rpc", async (route) => {
    const request = route.request().postDataJSON();
    const result = request.params.name === "list_projects" ? {
      projects: ["alpha", "beta"].map((name) => ({
        name, root_path: `/fixture/${name}`,
        schema: { node_labels: [{ label: "Function", count: 3 }], edge_types: [{ type: "CALLS", count: 1 }] },
      })),
    } : {};
    await route.fulfill({ json: { jsonrpc: "2.0", id: request.id,
      result: { content: [{ type: "text", text: JSON.stringify(result) }] } } });
  });

  const checkCanvas = async (count: number) => {
    await expect(page.getByText(`${count} nodes / 1 edges`, { exact: true })).toBeVisible();
    await expect(page.locator("canvas")).toHaveCount(1);
    await expect.poll(() => page.locator("canvas").evaluate((canvas: HTMLCanvasElement) => {
      const gl = canvas.getContext("webgl2");
      return !!gl && !gl.isContextLost() && gl.drawingBufferWidth > 0;
    })).toBe(true);
    expect(errors).toEqual([]);
  };

  await page.goto("/?tab=graph&project=alpha");
  await checkCanvas(2);
  await page.getByRole("button", { name: /^Function/ }).click();
  await expect(page.getByText("All nodes filtered out", { exact: true })).toBeVisible();
  await expect(page.locator("canvas")).toHaveCount(0);
  await page.getByRole("button", { name: "Reset Filters", exact: true }).click();
  await checkCanvas(2);
  await page.reload();
  await checkCanvas(2);
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await checkCanvas(2);
  await expect.poll(() => layouts.filter((p) => p === "alpha").length).toBeGreaterThanOrEqual(3);

  for (const index of [1, 0, 1]) {
    await page.getByRole("button", { name: "Projects", exact: true }).click();
    await expect(page.locator("canvas")).toHaveCount(0);
    await page.getByRole("button", { name: "View Graph", exact: true }).nth(index).click();
    await checkCanvas(index === 0 ? 2 : 3);
  }
  expect(layouts.at(-1)).toBe("beta");
  await page.close();
});
