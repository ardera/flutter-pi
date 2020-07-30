#ifndef _AOT_H
#define _AOT_H

#if defined(__cplusplus)
extern "C" {
#endif
/* AOT START*/
void* _kDartIsolateSnapshotData = NULL;
void* _kDartIsolateSnapshotInstructions = NULL;
void* _kDartVmSnapshotData = NULL;
void* _kDartVmSnapshotInstructions = NULL;

long _kDartIsolateSnapshotDataSize = 0;
long _kDartIsolateSnapshotInstructionsSize = 0;
long _kDartVmSnapshotDataSize = 0;
long _kDartVmSnapshotInstructionsSize = 0;
/* AOT END */
#if defined(__cplusplus)
}
#endif

#endif
