// SPDX-License-Identifier: AGPL-3.0-or-later
// Print one bundle file to stdout (exit 3 if absent). Lets the synchronous
// Node runner fetch through the same disk cache as the dev server.
import { getFile } from "./bundle.ts";
const f = await getFile(process.argv[2]);
if (!f) process.exit(3);
process.stdout.write(f.data);
