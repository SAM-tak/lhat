// elk.bundled.js carries no types of its own.
declare module "elkjs/lib/elk.bundled.js" {
    const ELK: new (options?: unknown) => {
        layout(graph: unknown): Promise<unknown>;
    };
    export default ELK;
}
