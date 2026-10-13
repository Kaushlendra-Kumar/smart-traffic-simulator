// The scenario library bundled into the static build (same files the server loads).

const files = import.meta.glob("../../../config/scenarios/*.json", {
  eager: true,
  query: "?raw",
  import: "default",
}) as Record<string, string>;

export interface BundledScenario {
  id: string;
  name: string;
  description: string;
  grid: boolean;
  text: string;
}

export const bundledScenarios: BundledScenario[] = Object.values(files)
  .map((text) => {
    const j = JSON.parse(text);
    return { id: j.id, name: j.name, description: j.description ?? "", grid: Boolean(j.network?.grid), text };
  })
  .sort((a, b) => a.id.localeCompare(b.id));

export const defaultScenarioId = "single-intersection";

export function scenarioText(id: string): string | undefined {
  return bundledScenarios.find((s) => s.id === id)?.text;
}
