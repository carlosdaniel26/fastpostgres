/** Wire kinds (see np_shm.h). */
export const enum Kind {
  Bool = 1,
  Int32 = 2,
  Int64 = 3,
  Float8 = 4,
  Text = 5,
  Int16 = 6,
  Float4 = 7,
}

/** A text column: raw bytes plus the int32 offsets between values. */
export interface TextColumn {
  bytes: Uint8Array;
  offsets: Int32Array;
}

export type ColumnValue =
  | Uint8Array // bool
  | Int16Array
  | Int32Array
  | BigInt64Array
  | Float32Array
  | Float64Array
  | TextColumn;

export interface Field {
  name: string;
  /** PostgreSQL type OID (informational). */
  pgtype: number;
  kind: number;
}

export interface Column extends Field {
  value: ColumnValue;
}

export interface Segment {
  rowCount: number;
  columns: Column[];
}

export type Row = Record<string, string | null>;
export type PushRow = Record<string, unknown>;

export type Handler = (field: Column, value: ColumnValue) => void;

export interface ReadOptions {
  /** Copy into Node-owned buffers instead of aliasing the segment. */
  copy?: boolean;
  handlers?: {
    onKind?: Record<number, Handler>;
    onColumn?: Record<string, Handler>;
  };
}

export interface ConnectionLike {
  /** Run a statement; returns row objects (nulls as `null`). */
  exec(sql: string): Row[];
  /** Stream rows with COPY FROM STDIN. */
  copyFrom(sql: string, data: string | Buffer | Array<Buffer | string>): void;
  close(): void;
}

export interface RingLike {
  /** Start the worker (one SQL call on `conn`) and wait until it is ready. */
  start(conn: ConnectionLike): void;
  /** Open a query; chunks land in `seg` (`/dev/shm/<seg>`). */
  open(seg: string, query: string): void;
  /** Fetch the next batch; returns the number of rows (0 = end of stream). */
  next(batch?: number): number;
  close(): void;
  /** Ingest `seg` into `table`; returns rows inserted. */
  write(seg: string, table: string): number;
  stop(): void;
  /** Push mode: run `query`; the worker streams rows as the executor produces. */
  pushOpen(query: string): void;
  /** Consume rows one by one, each already a `{ column: value }` object. */
  pushRows(handler: (row: PushRow) => void): void;
}

/** Shape of the compiled N-API addon. */
export interface NativeModule {
  Connection: new (conninfo?: string) => ConnectionLike;
  Ring: new (seg: string) => RingLike;
  read(name: string, options?: ReadOptions): Segment;
  writeSegment(name: string, columns: Column[]): number;
  synthSegment(name: string, n: number): number;
  synthCopy(n: number): Buffer[];
}
