/**
 * Node client for nativepostgress.
 *
 * `read()` is zero-copy by default: the typed arrays alias the mapped
 * `/dev/shm` segment, so they are only valid until that segment is rewritten
 * (e.g. the next `native_stream_next`). Use `{ copy: true }` to detach.
 */

/** Wire kind (see np_shm.h). */
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

export type Handler = (field: Column, value: ColumnValue) => void;

export interface ReadOptions {
  /** Copy into Node-owned buffers instead of aliasing the segment. */
  copy?: boolean;
  handlers?: {
    onKind?: Record<number, Handler>;
    onColumn?: Record<string, Handler>;
  };
}

/** A row from `Connection.exec`, keyed by column name. */
export type Row = Record<string, string | null>;

/** A libpq connection for the control channel. */
export class Connection {
  constructor(conninfo?: string);
  /** Run a statement; returns row objects (nulls as `null`). */
  exec(sql: string): Row[];
  /** Stream rows with COPY FROM STDIN. */
  copyFrom(sql: string, data: string | Buffer | Array<Buffer | string>): void;
  close(): void;
}

/** View a segment as typed arrays (zero-copy unless `copy`). */
export function read(name: string, options?: ReadOptions): Segment;

/** Encode columns (JS typed arrays) into a segment. Returns the byte size. */
export function writeSegment(name: string, columns: Column[]): number;

/** Generate the benchmark's rows straight into a segment. Returns the byte size. */
export function synthSegment(name: string, n: number): number;

/** Generate the benchmark's COPY payload. */
export function synthCopy(n: number): Buffer[];
