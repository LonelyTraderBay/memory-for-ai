import { useCallback, useEffect, useRef, useState } from "react";
import { callTool } from "../api/rpc";
import type { Project, SchemaInfo } from "../lib/types";

interface ProjectInfo {
  project: Project;
  schema: SchemaInfo | null;
}

interface UseProjectsResult {
  projects: ProjectInfo[];
  loading: boolean;
  error: string | null;
  refresh: () => void;
}

function isAbortError(error: unknown): boolean {
  return error instanceof Error && error.name === "AbortError";
}

export function useProjects(): UseProjectsResult {
  const [projects, setProjects] = useState<ProjectInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const requestSequence = useRef(0);
  const activeController = useRef<AbortController | null>(null);

  const fetchProjects = useCallback(async () => {
    activeController.current?.abort();
    const controller = new AbortController();
    activeController.current = controller;
    const sequence = ++requestSequence.current;
    setLoading(true);
    setError(null);
    try {
      const result = await callTool<{ projects: Project[] }>(
        "list_projects",
        { include_details: true },
        { signal: controller.signal },
      );
      const list = result.projects ?? [];

      /* include_details carries a count-only schema, avoiding an N+1
       * get_graph_schema burst while preserving the existing view model. */
      const infos: ProjectInfo[] = list.map((project) => ({
        project,
        schema: project.schema ?? null,
      }));

      if (requestSequence.current === sequence) setProjects(infos);
    } catch (e) {
      if (requestSequence.current === sequence && !isAbortError(e)) {
        setError(e instanceof Error ? e.message : "Failed to fetch projects");
      }
    } finally {
      if (requestSequence.current === sequence) {
        activeController.current = null;
        setLoading(false);
      }
    }
  }, []);

  useEffect(() => {
    fetchProjects();
    return () => {
      activeController.current?.abort();
      activeController.current = null;
      requestSequence.current += 1;
    };
  }, [fetchProjects]);

  return { projects, loading, error, refresh: fetchProjects };
}
