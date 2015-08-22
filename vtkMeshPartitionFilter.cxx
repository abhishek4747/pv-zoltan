/*=========================================================================

  Module                  : vtkMeshPartitionFilter.cxx

  Copyright (C) CSCS - Swiss National Supercomputing Centre.
  You may use modify and and distribute this code freely providing
  1) This copyright notice appears on all copies of source code
  2) An acknowledgment appears with any substantial usage of the code
  3) If this code is contributed to any other open source project, it
  must not be reformatted such that the indentation, bracketing or
  overall style is modified significantly.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

=========================================================================*/
//
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkPolyData.h"
#include "vtkUnstructuredGrid.h"
#include "vtkDataSetAttributes.h"
#include "vtkPointData.h"
#include "vtkCellData.h"
#include "vtkCellArray.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkSmartPointer.h"
#include "vtkTimerLog.h"
#include "vtkIdTypeArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkBoundingBox.h"
#include "vtkMath.h"
#include "vtkPointLocator.h"
#include "vtkPKdTree.h"
//
// For PARAVIEW_USE_MPI
#include "vtkPVConfig.h"
#ifdef PARAVIEW_USE_MPI
  #include "vtkMPI.h"
  #include "vtkMPIController.h"
  #include "vtkMPICommunicator.h"
#endif
#include "vtkDummyController.h"
//
#include "vtkBoundsExtentTranslator.h"
#include "vtkMeshPartitionFilter.h"
//
#include <sstream>
//
#define _USE_MATH_DEFINES
#include <math.h>
#include <float.h>
#include <numeric>
#include <algorithm>

#define my_debug(a) std::cout<< a <<" >>> "<<this->UpdatePiece<<std::endl

//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkMeshPartitionFilter);
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
// Function Type: Pre migration callback for cell exchange
// the cell exchange is controlled manually. We declare what we need to exchange
// and use this function to ensure all memory is allocated correctly for the receive
// to take place and unpack each object
//----------------------------------------------------------------------------
template <typename T>
void vtkMeshPartitionFilter::zoltan_pre_migrate_function_cell(void *data, int num_gid_entries, int num_lid_entries,
  int num_import, ZOLTAN_ID_PTR import_global_ids, ZOLTAN_ID_PTR import_local_ids,
  int *import_procs, int *import_to_part, int num_export, ZOLTAN_ID_PTR export_global_ids,
  ZOLTAN_ID_PTR export_local_ids, int *export_procs, int *export_to_part, int *ierr)
{
  CallbackData *callbackdata = static_cast<CallbackData*>(data);
  
  vtkIdType uniqueSends = 0;
  for (vtkIdType i=0; i<num_export; i++) {
    vtkIdType GID = export_global_ids[i];
    vtkIdType LID = GID - callbackdata->ProcessOffsetsCellId[callbackdata->ProcessRank];
    if (LID<0 || LID>callbackdata->RemoteGhost.size()){
      std::cout<<"SOMETHING VERY BAD: cells:"<<callbackdata->Input->GetNumberOfCells()<<"\tLID: "<<LID<<"\tRemoteGhostSize:"<<callbackdata->RemoteGhost.size()<<std::endl;
    }
    if (callbackdata->LocalToLocalIdMap[LID]==0 && callbackdata->RemoteGhost[LID]!=1) {
      callbackdata->LocalToLocalIdMap[LID] = -1;
      uniqueSends++;
    }
  }
  
  // How many cells will we have at the end
  vtkIdType OutputNumberOfLocalCells = callbackdata->Input->GetNumberOfCells();
  vtkIdType OutputNumberOfFinalCells = OutputNumberOfLocalCells + num_import - uniqueSends + callbackdata->LocalIdsToKeep.size();
  callbackdata->OutputPointsData     = callbackdata->Output->GetPoints()->GetData()->GetVoidPointer(0);

  // we'll need enough space to handle the largest cells
  callbackdata->MaxCellSize     = callbackdata->Input->GetMaxCellSize();
  callbackdata->OutputCellArray = vtkSmartPointer<vtkCellArray>::New();
  callbackdata->OutputCellArray->Allocate(OutputNumberOfLocalCells*(callbackdata->MaxCellSize+1));
  //
  vtkCellData *inCD  = callbackdata->Input->GetCellData();
  vtkCellData *outCD = callbackdata->Output->GetCellData();
  outCD->CopyAllocate(inCD, OutputNumberOfFinalCells);
  //
  callbackdata->self->InitializeFieldDataArrayPointers(callbackdata, inCD, outCD, OutputNumberOfFinalCells);

  std::vector<int> local(OutputNumberOfLocalCells, 1);
  for (vtkIdType i=0; i<num_export; i++) {
    vtkIdType this_lid = export_global_ids[i]-callbackdata->ProcessOffsetsCellId[callbackdata->ProcessRank];
    local[this_lid] = callbackdata->RemoteGhost[this_lid];
  }
  for (vtkIdType i=0; i<callbackdata->LocalIdsToKeep.size(); i++) {
    if (local[callbackdata->LocalIdsToKeep[i]] ==0)
      local[callbackdata->LocalIdsToKeep[i]] = 2;
    else
      std::cout<<"already local "<<callbackdata->LocalIdsToKeep[i]<<std::endl;
  }
  //
  vtkIdType npts, *pts, newpts[32];
  vtkPolyData         *pdata = vtkPolyData::SafeDownCast(callbackdata->Input);
//  vtkUnstructuredGrid *udata = vtkUnstructuredGrid::SafeDownCast(callbackdata->Input);
  vtkPolyData         *pdata2 = vtkPolyData::SafeDownCast(callbackdata->Output);
//  vtkUnstructuredGrid *udata2 = vtkUnstructuredGrid::SafeDownCast(callbackdata->Output);
  if (pdata2) {
    pdata2->SetPolys(callbackdata->OutputCellArray);
  }
  callbackdata->LocalToLocalCellMap.assign(OutputNumberOfLocalCells, -1);
  //
  callbackdata->OutCellCount = 0;
  for (vtkIdType cellId=0; cellId<OutputNumberOfLocalCells; cellId++) {
    if (local[cellId]) {
      // copy cell data from old to new datasets
      outCD->CopyData(inCD, cellId, callbackdata->OutCellCount);
      // copy cell point Ids to new dataset, 
      if (pdata) { 
        int ctype = pdata->GetCellType(cellId);
        pdata->GetCellPoints(cellId, npts, pts); 
        for (int i=0; i<npts; i++) {
          if (callbackdata->LocalToLocalIdMap[pts[i]]!=-1) newpts[i] = callbackdata->LocalToLocalIdMap[pts[i]];
          else {
            // wtf?
          }
        }
        pdata2->InsertNextCell(ctype, npts, newpts);
      }
      //else if (udata) { 
      //  int ctype = udata->GetCellType(cellId);
      //  udata->GetCellPoints(cellId, npts, pts); 
      //  udata2->InsertNextCell(ctype, npts, pts);
      //}
      callbackdata->LocalToLocalCellMap[cellId] = callbackdata->OutCellCount;
      callbackdata->OutCellCount++;
    }
  }
  if (callbackdata->OutCellCount!=OutputNumberOfFinalCells-num_import) {
    std::cout<<"VERY WRONG: "<<OutputNumberOfLocalCells-callbackdata->OutCellCount-num_import<<std::endl;
  }
  // make sure final point count is actual point count, not the reserved amount
//  callbackdata->Output->GetPoints()->SetNumberOfPoints(callbackdata->OutPointCount);
}
//----------------------------------------------------------------------------
// Zoltan callback function : returns size of each cell and all its data
//----------------------------------------------------------------------------
int vtkMeshPartitionFilter::zoltan_obj_size_function_cell(void *data, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_id, ZOLTAN_ID_PTR local_id, int *ierr)
{
  INC_SIZE_COUNT
  VTK_ZOLTAN_PARTITION_FILTER::CallbackData *callbackdata = (VTK_ZOLTAN_PARTITION_FILTER::CallbackData*)data;
  *ierr = ZOLTAN_OK;
  // return the size of the cell data + number of points in cell + point Ids
  vtkIdType GID = *global_id;
  vtkIdType LID = GID - callbackdata->ProcessOffsetsCellId[callbackdata->ProcessRank];
  //
  vtkIdType npts, *pts;
  vtkPolyData         *pdata = vtkPolyData::SafeDownCast(callbackdata->Input);
//  vtkUnstructuredGrid *udata = vtkUnstructuredGrid::SafeDownCast(callbackdata->Input);
  //
  int ctype = pdata->GetCellType(LID);
  pdata->GetCellPoints(LID,npts,pts);
  int size = 2 + npts; // npts + ctype + pts
  return callbackdata->TotalSizePerId + size*sizeof(vtkIdType);
}
//----------------------------------------------------------------------------
// Zoltan callback function : pack one cell and all its data
//----------------------------------------------------------------------------
void vtkMeshPartitionFilter::zoltan_pack_obj_function_cell(void *data, int num_gid_entries, int num_lid_entries,
  ZOLTAN_ID_PTR global_id, ZOLTAN_ID_PTR local_id, int dest, int size, char *buf, int *ierr)
{
  INC_PACK_COUNT
  VTK_ZOLTAN_PARTITION_FILTER::CallbackData *callbackdata = (VTK_ZOLTAN_PARTITION_FILTER::CallbackData*)data;
  vtkIdType GID = *global_id;
  vtkIdType LID = GID - callbackdata->ProcessOffsetsCellId[callbackdata->ProcessRank];
  //
  //
  vtkIdType npts, *pts, newPts[32];
  vtkPolyData         *pdata = vtkPolyData::SafeDownCast(callbackdata->Input);
//  vtkUnstructuredGrid *udata = vtkUnstructuredGrid::SafeDownCast(callbackdata->Input);
  //
  // Copy all Cell data arrays into the buffer provided by zoltan
  //
  for (int i=0; i<callbackdata->NumberOfFields; i++) {
    int asize = callbackdata->MemoryPerTuple[i];
    char *dataptr = (char*)(callbackdata->InputArrayPointers[i]) + asize*LID;
    memcpy(buf, dataptr, asize);
    buf += asize;
  }
  //
  // copy the cell point Ids into the buffer provided by zoltan.
  // before sending, we will convert local to global Ids because when the cell is unpacked on the 
  // remote process, the local Ids are meaningless - there is no way to find where they really came from 
  //
  if (pdata) {
    pdata->GetCellPoints(LID, npts, pts);
    // copy the number of points and cell type so we know what's been sent when we unpack
    newPts[0] = npts;
    newPts[1] = pdata->GetCellType(LID);
    // and the points Ids converted to global Ids
    for (int i=0; i<npts; i++) {
      newPts[i+2] = pts[i] + callbackdata->ProcessOffsetsPointId[callbackdata->ProcessRank];
    }
    memcpy(buf, newPts, sizeof(vtkIdType)*(npts+2));  
  }
  //else if (udata) { 
  //  throw std::string("Implement this");
  //}

  *ierr = ZOLTAN_OK;
  return;
}
//----------------------------------------------------------------------------
// Zoltan callback function : unpack one cell and all its data
//----------------------------------------------------------------------------
void vtkMeshPartitionFilter::zoltan_unpack_obj_function_cell(void *data, int num_gid_entries,
  ZOLTAN_ID_PTR global_id, int size, char *buf, int *ierr)
{
  INC_UNPACK_COUNT
  VTK_ZOLTAN_PARTITION_FILTER::CallbackData *callbackdata = (VTK_ZOLTAN_PARTITION_FILTER::CallbackData*)data;
  //
  vtkIdType GID = *global_id;
//  vtkIdType LID = GID - callbackdata->ProcessOffsetsCellId[callbackdata->ProcessRank];
  //
  vtkIdType npts, *pts, ctype, newPts[32];
  vtkPolyData         *pdata = vtkPolyData::SafeDownCast(callbackdata->Input);
//  vtkUnstructuredGrid *udata = vtkUnstructuredGrid::SafeDownCast(callbackdata->Input);
  vtkPolyData         *pdata2 = vtkPolyData::SafeDownCast(callbackdata->Output);
//  vtkUnstructuredGrid *udata2 = vtkUnstructuredGrid::SafeDownCast(callbackdata->Output);
  //
  for (int i=0; i<callbackdata->NumberOfFields; i++) {
    int asize = callbackdata->MemoryPerTuple[i];
    char *dataptr = (char*)(callbackdata->OutputArrayPointers[i]) + asize*(callbackdata->OutCellCount);
    memcpy(dataptr, buf, asize);
    buf += asize;
  }
  // we have received a cell with a list of Global point Ids, 
  // We need to convert each of the global Ids to the local Ids they became 
  // when they were copied into the local point list
  npts  =  reinterpret_cast<vtkIdType*>(buf)[0];
  ctype =  reinterpret_cast<vtkIdType*>(buf)[1];
  pts   = &reinterpret_cast<vtkIdType*>(buf)[2];
  for (int i=0; i<npts; i++) {
    newPts[i] = callbackdata->self->global_to_local_Id(pts[i]);
  }
  if (pdata) {
    pdata2->InsertNextCell(ctype, npts, newPts);
  }
  //else if (udata) { 
  //  throw std::string("Implement this");
  //}

  callbackdata->OutCellCount++;
  *ierr = ZOLTAN_OK;
  return;
}
//----------------------------------------------------------------------------
// vtkMeshPartitionFilter :: implementation 
//----------------------------------------------------------------------------
vtkMeshPartitionFilter::vtkMeshPartitionFilter()
{
  this->GhostMode           = vtkMeshPartitionFilter::None;
  this->GhostCellOverlap    = 0.0;
  this->NumberOfGhostLevels = 0;
  this->ghost_array         = NULL;
}
//----------------------------------------------------------------------------
vtkMeshPartitionFilter::~vtkMeshPartitionFilter()
{
  if (this->ZoltanData) {
    Zoltan_Destroy(&this->ZoltanData);
  }
}
//----------------------------------------------------------------------------
void vtkMeshPartitionFilter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}
//----------------------------------------------------------------------------
int vtkMeshPartitionFilter::RequestData(vtkInformation* info,
                                 vtkInformationVector** inputVector,
                                 vtkInformationVector* outputVector)
{
  //
  // Calculate even distribution of points across processes
  // This step only performs the load balance analysis, 
  // no actual sending of data takes place yet.
  //
  this->PartitionPoints(info, inputVector, outputVector);

  if (this->UpdateNumPieces==1) {
    // input has been copied to output during PartitionPoints
    return 1;
  }

  //
  // based on the point partition, decide which cells need to be sent away
  // sending some cells may imply sending a few extra points too
  //

  vtkDebugMacro(<<"Entering BuildCellToProcessList");
  if (this->ZoltanCallbackData.PointType==VTK_FLOAT) {
    this->BuildCellToProcessList<float>(this->ZoltanCallbackData.Input, 
      this->cell_partitioninfo,       // lists of which cells to send to which process
      this->MigrateLists.known, // list of which points to send to which process
      this->LoadBalanceData     // the partition information generated during PartitionPoints
    );
  }
  else if (this->ZoltanCallbackData.PointType==VTK_DOUBLE) {
    this->BuildCellToProcessList<double>(this->ZoltanCallbackData.Input, 
      this->cell_partitioninfo,       // lists of which cells to send to which process
      this->MigrateLists.known, // list of which points to send to which process
      this->LoadBalanceData     // the partition information generated during PartitionPoints
    );
  }

  //*****************************************************************
  // Free the arrays allocated by Zoltan_LB_Partition
  // before we do a manual migration.
  //*****************************************************************
  vtkDebugMacro(<<"Freeing Zoltan LB arrays");
  // Zoltan_LB_Free_Part(&this->LoadBalanceData.importGlobalGids, &this->LoadBalanceData.importLocalGids, &this->LoadBalanceData.importProcs, &this->LoadBalanceData.importToPart);
  // Zoltan_LB_Free_Part(&this->LoadBalanceData.exportGlobalGids, &this->LoadBalanceData.exportLocalGids, &this->LoadBalanceData.exportProcs, &this->LoadBalanceData.exportToPart);

  //
  // Based on the original partition and our extra cell point allocations
  // perform the main point exchange between all processes
  //
  this->ZoltanCallbackData.RemoteGhost = this->MigrateLists.known.RemoteGhost;
  this->ComputeInvertLists(this->MigrateLists);
  this->ManualPointMigrate(this->MigrateLists, this->KeepInversePointLists==1);
  
  if (!this->KeepInversePointLists) {
    vtkDebugMacro(<<"Release point exchange data");
    this->MigrateLists.known.GlobalIds.clear();
    this->MigrateLists.known.Procs.clear();
    this->MigrateLists.known.LocalIdsToKeep.clear();
  }
  this->ZoltanCallbackData.LocalIdsToKeep = cell_partitioninfo.LocalIdsToKeep;
  this->ZoltanCallbackData.LocalIdsToSend = cell_partitioninfo.LocalIdsToSend;
  this->ZoltanCallbackData.RemoteGhost = cell_partitioninfo.RemoteGhost;

my_debug("Partition of Cell starting.");
  // after deleting memory, add a barrier to let ranks free as much as possible before the next big allocation
  this->Controller->Barrier();
  //
  // Distribute cells based on the usage of the points already distributed
  //
  this->PartitionCells(this->cell_partitioninfo);
  
  my_debug("Partition of Cell complete.");
  
  // if we are generating ghost cells for the mesh, then we must allocate a new array
  // on the output to store the ghost cell information (level 0,1,2...N ) etc
  vtkIdType numCells = this->ZoltanCallbackData.Output->GetNumberOfCells();
  if (this->GhostMode!=vtkMeshPartitionFilter::None) {
    my_debug("Created a ghost array with " << numCells);
    this->ghost_array = vtkUnsignedCharArray::New();
    this->ghost_array->SetName("vtkGhostLevels");
    this->ghost_array->SetNumberOfTuples(numCells);
    this->ZoltanCallbackData.Output->GetCellData()->AddArray(this->ghost_array);
    for (int i=0; i<numCells; i++) {
      this->ghost_array->SetTuple1(i, 0);
    }
    
    for (int i=0;i<cell_partitioninfo.LocalIdsToKeep.size(); i++) {
      this->ghost_array->SetTuple1(this->ZoltanCallbackData.LocalToLocalCellMap[cell_partitioninfo.LocalIdsToKeep[i]], cell_partitioninfo.LocalIdsToSend[i]);
    }
  }

  
  
  if (this->InputDisposable) {
    vtkDebugMacro(<<"Disposing of input points and point data");
    this->ZoltanCallbackData.Input->SetPoints(NULL);
    this->ZoltanCallbackData.Input->GetPointData()->Initialize();
  }

  this->cell_partitioninfo.RemoteGhost.clear();
  this->MigrateLists.known.RemoteGhost.clear();
  
#ifdef VTK_ZOLTAN2_PARTITION_FILTER
  cell_partitioninfo.LocalIdsToSend.clear();
#endif
  cell_partitioninfo.LocalIdsToKeep.clear();
  //
  // build a tree of bounding boxes to use for rendering info/hints or other spatial tests
  //

  // this->CreatePkdTree();
  // this->ExtentTranslator->SetKdTree(this->GetKdtree());


  
  
  my_debug("Ghost Cells marked");
  //*****************************************************************
  // Free the storage allocated for the Zoltan structure.
  //*****************************************************************
  if (!this->KeepInversePointLists) {
    Zoltan_Destroy(&this->ZoltanData);
  }
  
  this->Timer->StopTimer();
  vtkDebugMacro(<<"Particle partitioning : " << this->Timer->GetElapsedTime() << " seconds");
  return 1;
}
//----------------------------------------------------------------------------
int vtkMeshPartitionFilter::PartitionCells(PartitionInfo &cell_partitioninfo)
{
  vtkDebugMacro(<<"Entering PartitionCells");

  //
  // now we have a map of cells to processId, so do a collective 'invert lists' 
  // operation to compute the global exchange map of who sends cells to who 
  //
  size_t        num_known = cell_partitioninfo.GlobalIds.size(); 
  int           num_found = 0;
  ZOLTAN_ID_PTR found_global_ids = NULL;
  ZOLTAN_ID_PTR found_local_ids  = NULL;
  int          *found_procs      = NULL;
  int          *found_to_part    = NULL;
  //
  vtkDebugMacro(<<"About to invert lists (cell migration)");
  int zoltan_error = Zoltan_Invert_Lists(this->ZoltanData, 
    (int)num_known,
    num_known>0 ? &cell_partitioninfo.GlobalIds[0] : NULL,
    NULL,
    num_known>0 ? &cell_partitioninfo.Procs[0]     : NULL,
    NULL,
    &num_found,
    &found_global_ids,
    &found_local_ids,
    &found_procs,
    &found_to_part); 
  //
  if (zoltan_error != ZOLTAN_OK){
    printf("Zoltan_LB_Partition NOT OK...\n");
    MPI_Finalize();
    Zoltan_Destroy(&this->ZoltanData);
    exit(0);
  }
  //
  //  make sure field arrays are setup and ready for migration/copying
  //
  this->AllocateFieldArrays(this->ZoltanCallbackData.Output->GetCellData());
  
  my_debug("Partitition Cell Inverst Lists done");

  //
  // we need to change the callback functions for migration to use cell based
  // functions we declare instead of the point exchange ones
  //
  zsize_fn  f1 = zoltan_obj_size_function_cell;
  zpack_fn  f2 = zoltan_pack_obj_function_cell;
  zupack_fn f3 = zoltan_unpack_obj_function_cell;
  zprem_fn  f4 = zoltan_pre_migrate_function_cell<float>;
  Zoltan_Set_Fn(this->ZoltanData, ZOLTAN_OBJ_SIZE_FN_TYPE,       (void (*)()) f1, &this->ZoltanCallbackData);
  Zoltan_Set_Fn(this->ZoltanData, ZOLTAN_PACK_OBJ_FN_TYPE,       (void (*)()) f2, &this->ZoltanCallbackData); 
  Zoltan_Set_Fn(this->ZoltanData, ZOLTAN_UNPACK_OBJ_FN_TYPE,     (void (*)()) f3, &this->ZoltanCallbackData); 
  Zoltan_Set_Fn(this->ZoltanData, ZOLTAN_PRE_MIGRATE_PP_FN_TYPE, (void (*)()) f4, &this->ZoltanCallbackData); 

  //
  // Perform the cell exchange
  //
  vtkDebugMacro(<<"About to Zoltan_Migrate (cells)");
  zoltan_error = Zoltan_Migrate (this->ZoltanData,
    (int)num_found,
    found_global_ids,
    found_local_ids,
    found_procs,
    found_to_part,
    (int)num_known,
    num_known>0 ? &cell_partitioninfo.GlobalIds[0] : NULL,
    NULL,
    num_known>0 ? &cell_partitioninfo.Procs[0]     : NULL,
    NULL
    );

  //
  // Release the arrays allocated during Zoltan_Invert_Lists
  //
  vtkDebugMacro(<<"About to Free Zoltan_Migrate (cells)");
  Zoltan_LB_Free_Part(
    &found_global_ids, 
    &found_local_ids, 
    &found_procs, 
    &found_to_part);
  vtkDebugMacro(<<"Done Migration (cells)");

  vtkDebugMacro(<<"Release pre-invert arrays (cells)");
  cell_partitioninfo.GlobalIds.clear();
  cell_partitioninfo.Procs.clear();
//  cell_partitioninfo.LocalIdsToKeep.clear();

  return 1;
}

//----------------------------------------------------------------------------
// 
// Build a list of cell to process Ids based on the already performed 
// point redistribution. Points which are being sent away will be taking
// their cells with them. Caution, some points are shared between cells and may 
// go to different processes. For these cells, we must send another copy 
// of the points to the relevant process.
//
template <typename T>
void vtkMeshPartitionFilter::BuildCellToProcessList(     
  vtkDataSet *data, 
  PartitionInfo &cell_partitioninfo, 
  PartitionInfo &point_partitioninfo, 
  ZoltanLoadBalanceData &loadBalanceData)
{
  vtkIdType numPts = data->GetNumberOfPoints();
  vtkIdType numCells = data->GetNumberOfCells();

  // we will put the results of the cell tests in these arrays
  cell_partitioninfo.Procs.reserve(numCells/this->UpdateNumPieces);
  cell_partitioninfo.GlobalIds.reserve(numCells/this->UpdateNumPieces);

  // we know that some points on this process will be exported to remote processes
  // so build a point to process map to quickly lookup the process Id from the point Id
  // 1) initially set all points as local to this process
  std::vector<int> localId_to_process_map(numPts, this->UpdatePiece); 
  // 2) loop over all to be exported and note the destination
  for (vtkIdType i=0; i<loadBalanceData.numExport; i++) {
    vtkIdType id               = loadBalanceData.exportGlobalGids[i] - this->ZoltanCallbackData.ProcessOffsetsPointId[this->ZoltanCallbackData.ProcessRank];
    localId_to_process_map[id] = loadBalanceData.exportProcs[i];
  }
  // the list now has the mapping from local ID to remote process, 
  // now we must examine each cell and see if they can be sent to a remote process
  // or if they are split between processes, when we find cells split across (multiple) remote processes, 
  // we may need to add some points to the send lists so that the whole cell is correctly represented.
  // This vector will store {Id,process} pairs wilst the list is being scanned
  typedef std::pair<vtkIdType, int> process_tuple;
  std::vector<process_tuple> process_vector;

  // some points are marked to be sent away, but we will also need to keep a copy
  // reserve a little space to get things going (1% of total exports to start)
  point_partitioninfo.LocalIdsToKeep.reserve(loadBalanceData.numExport/100);

  // we must handle Polydata and UnstructuredGrid slightly differently
  vtkIdType npts, *pts;
  vtkPolyData         *pdata = vtkPolyData::SafeDownCast(data);
  vtkUnstructuredGrid *udata = vtkUnstructuredGrid::SafeDownCast(data);
  if (!pdata && !udata) {
    vtkErrorMacro(<<"Only Polydata and UnstructuredGrid supported so far");  
  }
  // polydata requires a cell map (verts/lines/polys/strips) to be present before we traverse cells
  if (pdata) pdata->BuildCells();

  // a simple bitmask with one entry per process, used for each cell to count processes for the cell
  std::vector<unsigned char> process_flag(this->UpdateNumPieces,0);;

  int LEVEL_MAX = this->NumberOfGhostLevels;
  int REMOTE_LEVEL = 2*LEVEL_MAX +2;
  int LOCAL_LEVEL = 0;
  int GHOST_LEVEL = LEVEL_MAX+1;
  std::vector<vtkIdType> cell_level_info;
  std::vector<int> cell_status;
  cell_level_info.resize(numCells);
  cell_status.resize(numCells);

      my_debug("cells: "<<numCells<<"\tpoints: "<<numPts<<"\t LEVEL_MAX: "<<LEVEL_MAX );
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // BOUNDING BOX
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  if (this->GhostMode==vtkMeshPartitionFilter::BoundingBox) {
    std::vector<int> point_to_level_map(numPts, -1);
    double bounds[6];
    data->GetBounds(bounds);

    vtkBoundingBox localBoundingBox(bounds);
    {
      int proc = this->UpdatePiece;
      vtkBoundingBox b      = vtkBoundingBox(this->BoxList[proc]);
      vtkBoundingBox b_out  = vtkBoundingBox(b); b_out.Inflate(this->GhostCellOverlap*LEVEL_MAX);
      vtkBoundingBox b_in   = vtkBoundingBox(b); b_in.Inflate(-this->GhostCellOverlap*LEVEL_MAX);
      for (vtkIdType i=0; i<numPts; i++) {
        double *pt = data->GetPoint(i);
        bool absolutelocalpoint = b_in.ContainsPoint(pt);
        bool localpoint         = b.ContainsPoint(pt);
        bool extendedlocalpoint = b_out.ContainsPoint(pt);
        if (point_to_level_map[i]!=-1) {
          my_debug("something wrong 2");
        }
        if (absolutelocalpoint) {
          point_to_level_map[i] = LOCAL_LEVEL;
        } else if (!extendedlocalpoint) {
          point_to_level_map[i] = REMOTE_LEVEL;
        }
        else {
          // point lies in ghost band
          // there is no GHOST_LEVEL in bounding box case... even number of ghost levels.. half inner... half outer
          for (int l=0; l<LEVEL_MAX; l++) {
            if (!localpoint) {
              vtkBoundingBox b_this = vtkBoundingBox(b); b_this.Inflate(this->GhostCellOverlap*l);
              vtkBoundingBox b_next = vtkBoundingBox(b); b_next.Inflate(this->GhostCellOverlap*(l+1));
              
              bool point_in_this = b_this.ContainsPoint(pt);
              bool point_in_next = b_next.ContainsPoint(pt);
              
              if (!point_in_this && point_in_next) {
                point_to_level_map[i] = GHOST_LEVEL + l + 1;
              }
            }else{
              vtkBoundingBox b_this = vtkBoundingBox(b); b_this.Inflate(-this->GhostCellOverlap*l);
              vtkBoundingBox b_next = vtkBoundingBox(b); b_next.Inflate(-this->GhostCellOverlap*(l+1));
              
              bool point_in_this = b_this.ContainsPoint(pt);
              bool point_in_next = b_next.ContainsPoint(pt);
              
              if (point_in_this && !point_in_next) {
                point_to_level_map[i] = GHOST_LEVEL - l - 1;
              }
            }
          }
        }
      }
    }
    std::vector<int> level_flag(REMOTE_LEVEL+1, 0);
    for (vtkIdType cellId=0; cellId<numCells; cellId++) {
      // get a pointer to the cell points
      if (pdata) { pdata->GetCellPoints(cellId, npts, pts); }
      else if (udata) { udata->GetCellPoints(cellId, npts, pts); }

      level_flag.assign(REMOTE_LEVEL+1, 0);
      for (int j=0; j<npts; j++) {
        int level = point_to_level_map[pts[j]];
        level_flag[level] += 1;
      }

      int max_level = LOCAL_LEVEL;
      for (int level = LOCAL_LEVEL; level<=REMOTE_LEVEL; level++) {
        if (level_flag[max_level]<level_flag[level]) {
          max_level = level;
        }
      }
      
//      max_level = point_to_level_map[pts[0]];
      cell_level_info[cellId] = max_level;
      
      process_flag.assign(this->UpdateNumPieces,0);
      int points_remote = 0;
      for (int j=0; j<npts; j++) {
        int process = localId_to_process_map[pts[j]];
        process_flag[process] = 1;
        if (process!=this->UpdatePiece) {
          points_remote++;
        }
      }
      
      cell_status[cellId] = 0;
      if (points_remote==0) {
        cell_status[cellId] = 1;  // All points of this cell are local
      }
      else {
        int process_count = std::count(process_flag.begin(), process_flag.end(), 1);
        // all points are on the same remote process
        if (process_count==1) {
          cell_status[cellId] = 3; // All points of this cell belong same remote process
        }
        // some local, others all on remote process(es)
        else if (process_count>1 && points_remote<npts) {
          cell_status[cellId] = 2;
        }
        // all on remote processes, but not all the same process
        else if (process_count>1 && points_remote==npts) {
          cell_status[cellId] = 4;
        }
        else {
          throw std::string("This should not be possible");
        }
      }

      //
      // We will use the process receiving the first point as the desired final location :
      // ideally we'd count them and choose the process with the most, but the gain would be tiny
      //
      vtkIdType destProcess = localId_to_process_map[pts[0]];
      if (cell_status[cellId]==1) {
        // If cell in ghost region then send it too
      }
      else if (cell_status[cellId]>=3 && destProcess!=this->UpdatePiece) {
        // The cell is going to be sent away, so add it to our send list
        cell_partitioninfo.Procs.push_back(destProcess);
        cell_partitioninfo.GlobalIds.push_back(cellId + this->ZoltanCallbackData.ProcessOffsetsCellId[this->UpdatePiece]);
        
        if (cell_level_info[cellId]>GHOST_LEVEL && cell_level_info[cellId]<REMOTE_LEVEL) {
          
          cell_partitioninfo.LocalIdsToKeep.push_back(cellId);
#ifdef VTK_ZOLTAN2_PARTITION_FILTER
          cell_partitioninfo.LocalIdsToSend.push_back(cell_level_info[cellId]-GHOST_LEVEL);
#endif
          for (int pc=0; pc<npts; pc++) {
            if (localId_to_process_map[pts[pc]]!=this->UpdatePiece) {
              point_partitioninfo.LocalIdsToKeep.push_back(pts[pc]);
            }
          }
        }
      }
      
      //
      // cells of type 2 and 4 need special treatment to handle the cell split over N processes
      //
      if (cell_status[cellId]==2 || cell_status[cellId]==4) {
        for (int i=0; i<npts; i++) {
          // the point is going to be sent away - but - we need to keep a copy locally
          if (cell_status[cellId]==2 && localId_to_process_map[pts[i]]!=this->UpdatePiece) {
            point_partitioninfo.LocalIdsToKeep.push_back(pts[i]);
          }
          // if the cell was split over multiple remote processes, we must send the points
          // needed to complete the cell to the correct process
          if (cell_status[cellId]==4 && localId_to_process_map[pts[i]] != destProcess) {
            // The point is going to be sent away, so add it to our send list
            process_vector.push_back( process_tuple(pts[i], destProcess) );
          }
        }
      }
      
      
      
    }
//    this->Controller->Barrier();
    for (int level = LOCAL_LEVEL; level <= REMOTE_LEVEL ; ++level)
    {
      my_debug("Level:"<<level
               <<"\t Points:"<<std::count(point_to_level_map.begin(), point_to_level_map.end(), level)
               <<"\t Cells:"<<std::count(cell_level_info.begin(), cell_level_info.end(), level) );
    }
  }
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // NEIGHBOR ALGO
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  else{
    // Level Classification
    // LEVEL_MAX is the no. of neighbouring ghost cells are required apart from one layer of ghost cells
    // default for LEVEL_MAX = 0
    // [LOCAL_LEVEL] [ ] ...[-2] [-1] [GHOST_LEVEL / LEVEL_MAX] [+1] [+2] ... [REMOTE_LEVEL]
    
    point_partitioninfo.RemoteGhost.assign(numPts, 0);
    cell_partitioninfo.RemoteGhost.assign(numCells, 0);
    // Creating few look up tables
    std::vector<std::vector<vtkIdType> > point_to_cell_map;
    std::vector<std::vector<vtkIdType> > cell_to_point_map;
    std::vector< std::vector<vtkIdType> > level_to_cell_map;

    if (LEVEL_MAX>0){
      point_to_cell_map.resize(numPts);
      cell_to_point_map.resize(numCells);
      level_to_cell_map.resize(REMOTE_LEVEL+1);
    }

    //
    // for each cell, find if all points are designated as remote and cell needs to be sent away
    //
    for (vtkIdType cellId=0; cellId<numCells; cellId++) {
      // get a pointer to the cell points
      if (pdata) { pdata->GetCellPoints(cellId, npts, pts); }
      else if (udata) { udata->GetCellPoints(cellId, npts, pts); }

      // Create our two lookup tables
      if (LEVEL_MAX>0){
        cell_to_point_map[cellId].resize(npts);
        for (int j = 0; j < npts; ++j){
          point_to_cell_map[pts[j]].push_back(cellId);
          cell_to_point_map[cellId][j] = pts[j];
        }
      }
      // Cell status: Classification
      //
      // we need to examine all points of the cell and classify it : there are several possibile actions
      //
      // 1) all points are local                     : keep cell
      // 2) some points are local, some remote       : keep cell, make sure any points marked for sending are kept locally too
      // 3) all points on same remote process        : send cell to remote process
      // 4) all points on different remote processes : send cell to one remote process, also add other points to send list for that process
      //
      // use a bit index to mark/mask processes receiving points
      process_flag.assign(this->UpdateNumPieces,0);
      int points_remote = 0;
      for (int j=0; j<npts; j++) {
        int process = localId_to_process_map[pts[j]];
        process_flag[process] = 1;
        if (process!=this->UpdatePiece) {
          points_remote++;
        }
      }
      
      cell_status[cellId] = 0;
      if (points_remote==0) {
        cell_status[cellId] = 1;  // All points of this cell are local
        cell_level_info[cellId] = LOCAL_LEVEL; // local cell
      }
      else {
        int process_count = std::count(process_flag.begin(), process_flag.end(), 1);
        // all points are on the same remote process
        if (process_count==1) {
          cell_status[cellId] = 3; // All points of this cell belong same remote process
          cell_level_info[cellId] = REMOTE_LEVEL; // remote cell
        }
        // some local, others all on remote process(es)
        else if (process_count>1 && points_remote<npts) {
          cell_status[cellId] = 2;
          cell_level_info[cellId] = GHOST_LEVEL; // ghost level 1 cell
        }
        // all on remote processes, but not all the same process
        else if (process_count>1 && points_remote==npts) {
          cell_status[cellId] = 4;
          cell_level_info[cellId] = REMOTE_LEVEL; // remote cell
        }
        else {
          throw std::string("This should not be possible");
        }
      }

      // Put level wise points in our map
      if (LEVEL_MAX>0)
        level_to_cell_map[cell_level_info[cellId]].push_back(cellId);

      //
      // We will use the process receiving the first point as the desired final location :
      // ideally we'd count them and choose the process with the most, but the gain would be tiny
      //
      vtkIdType destProcess = localId_to_process_map[pts[0]];
      if (cell_status[cellId]>=3) {
        // The cell is going to be sent away, so add it to our send list
        cell_partitioninfo.Procs.push_back(destProcess); 
        cell_partitioninfo.GlobalIds.push_back(cellId + this->ZoltanCallbackData.ProcessOffsetsCellId[this->UpdatePiece]);
      }

      //
      // cells of type 2 and 4 need special treatment to handle the cell split over N processes
      //
      if (cell_status[cellId]==2 || cell_status[cellId]==4) {
        if (cell_status[cellId]==2) {
          if (this->UpdatePiece!=destProcess){
            cell_partitioninfo.Procs.push_back(destProcess);
            cell_partitioninfo.GlobalIds.push_back(cellId + this->ZoltanCallbackData.ProcessOffsetsCellId[this->UpdatePiece]);
            cell_partitioninfo.RemoteGhost[cellId] = 1;
          }
          for (int i=0; i<npts; i++) {
            if (localId_to_process_map[pts[i]]!=this->UpdatePiece)
              point_partitioninfo.LocalIdsToKeep.push_back(pts[i]);

//            else if ( this->UpdatePiece==0){
//            }
            if (this->UpdatePiece!=destProcess){
              if (destProcess!= localId_to_process_map[pts[i]]){
                process_vector.push_back(process_tuple(pts[i], destProcess));
                if (this->UpdatePiece==localId_to_process_map[pts[i]])
                  point_partitioninfo.RemoteGhost[pts[i]] = 1;
              }
            }

          }
          if (this->UpdatePiece==0)
          my_debug("Sending "<<cellId<<" extra to "<<destProcess);
        }
        if (cell_status[cellId]==4){
          for (int i=0; i<npts; i++) {
            // the point is going to be sent away - but - we need to keep a copy locally
            // if the cell was split over multiple remote processes, we must send the points
            // needed to complete the cell to the correct process
            if (cell_status[cellId]==4 && localId_to_process_map[pts[i]] != destProcess) {
              // The point is going to be sent away, so add it to our send list
              process_vector.push_back( process_tuple(pts[i], destProcess) );
            }
          }
        }
      }
    }


    if (LEVEL_MAX>0){
      my_debug("Ghost Cells: "<<level_to_cell_map[GHOST_LEVEL].size()<<"\t Local: "<<level_to_cell_map[LOCAL_LEVEL].size()<<"\t Remote: "<<level_to_cell_map[REMOTE_LEVEL].size());
      // Next Level Ghost Cells : Points to be kept
      // Start from the ghost level and move towards remote level
      for (int level = GHOST_LEVEL; level < REMOTE_LEVEL-1; ++level)
      {
        std::vector<vtkIdType> next_level_cells;
//  #define USE_VTK_ALGO_FOR_NEIGHBORS
  #ifdef  USE_VTK_ALGO_FOR_NEIGHBORS
        // Doesn't work
        pdata->BuildLinks();
        
        this->Timer->StartTimer();
        for (int cell_id = 0;  cell_id < level_to_cell_map[level].size(); ++cell_id) {
          // Cell ID
          vtkIdType cellId = level_to_cell_map[level][cell_id];
          
          // Get points belonging to this cell
          vtkSmartPointer<vtkIdList> cellPointIds = vtkSmartPointer<vtkIdList>::New();
          if (pdata) { pdata->GetCellPoints(cellId, cellPointIds); }
          else if (udata) { udata->GetCellPoints(cellId, cellPointIds); }
          
          
          //get the neighbors of the cell
          vtkSmartPointer<vtkIdList> neighborCellIds = vtkSmartPointer<vtkIdList>::New();
          if (pdata) { pdata->GetCellNeighbors(cellId, cellPointIds, neighborCellIds); }
          else if (udata){ udata->GetCellNeighbors(cellId, cellPointIds, neighborCellIds); }
          else{ my_debug("This shouldn't happen."); }
          
          for (int j =0;  j < neighborCellIds->GetNumberOfIds(); j++) {
            next_level_cells.push_back(neighborCellIds->GetId(j));
          }
        }
        this->Timer->StopTimer();
        std::cout<<"Algo: vtk\t";
  #else
        this->Timer->StartTimer();
        next_level_cells.resize(0);
        // For all cell at this level
        for (int i = 0; i < level_to_cell_map[level].size(); ++i)
        {
          vtkIdType cellId = level_to_cell_map[level][i];
            std::vector<vtkIdType> pts_(cell_to_point_map[cellId]);
            int npts_ = pts_.size();

          // Find the neighbouring cell for each point and collect them in next_level_cells
          for (int j = 0; j < npts_; ++j)
          {
            std::vector<vtkIdType> new_cells(point_to_cell_map[pts_[j]]);
//            std::vector<vtkIdType> new_cells(point_to_cell_map[pts_[j]].size(),-1);
//            for (int k=0; k<point_to_cell_map[pts_[j]].size(); k++) {
//              new_cells[k] = point_to_cell_map[pts_[j]][k];
//            }
              next_level_cells.insert(next_level_cells.end(), new_cells.begin(), new_cells.end());
          }
        }
        this->Timer->StopTimer();
        std::cout<<"Algo: brute force\t";
        
  #endif
        
        // Find unique next level cell
        std::sort(next_level_cells.begin(), next_level_cells.end());
        next_level_cells.erase(std::unique(next_level_cells.begin(), next_level_cells.end()), next_level_cells.end() );

        my_debug("Finding Neighbor Time: "<<this->Timer->GetElapsedTime()<<"\t Neighbors:"<<next_level_cells.size());

        level_to_cell_map[level+1].resize(0);
        // For each neighboring cell
        for (int j = 0; j <  next_level_cells.size(); ++j)
        {
          vtkIdType neighborCellId = next_level_cells[j];

            // If it is a remote cell 
            if (cell_level_info[neighborCellId]==REMOTE_LEVEL)
            {
              // first erase from old level list, assign it a new level then add to new level list
              level_to_cell_map[REMOTE_LEVEL].erase(std::find(level_to_cell_map[REMOTE_LEVEL].begin(), level_to_cell_map[REMOTE_LEVEL].end(), neighborCellId));
              cell_level_info[neighborCellId] = level+1;  
              level_to_cell_map[cell_level_info[neighborCellId]].push_back(neighborCellId);
              
            }
        }
      }

      
      // Previous Level Ghost Cells : Points to be sent
      // Start from the ghost level and move towards local level
      for (int level = GHOST_LEVEL; level > LOCAL_LEVEL+1 && false; --level)
      {
        std::vector<process_tuple> next_level_cells;
        // For all cell at this level
        for (int cell_id = 0; cell_id < level_to_cell_map[level].size(); ++cell_id)
        {
          vtkIdType cellId = level_to_cell_map[level][cell_id];
          std::vector<vtkIdType> pts_(cell_to_point_map[cellId]);
          int npts_ = pts_.size();

          // Find the neighbouring cell for each point and collect them in next_level_cells
          for (int j = 0; j < npts_; ++j)
          {
            std::vector<process_tuple> new_cells;
//            new_cells.resize(point_to_cell_map[pts_[j]].size());
            for (int k=0; k<point_to_cell_map[pts_[j]].size(); k++) {
              new_cells.push_back(process_tuple(point_to_cell_map[pts_[j]][k], localId_to_process_map[pts_[j]]));
            }
            next_level_cells.insert(next_level_cells.end(), new_cells.begin(), new_cells.end());
          }
        }

        // Find unique next level cell
        std::sort(next_level_cells.begin(), next_level_cells.end());
        next_level_cells.erase(std::unique(next_level_cells.begin(), next_level_cells.end()), next_level_cells.end() );

        // For each neighboring cell
        for (int j = 0; j <  next_level_cells.size(); ++j)
        {
          vtkIdType neighborCellId = next_level_cells[j].first;
          vtkIdType destProcess = next_level_cells[j].second;

          // If it is a local cell 
          if (cell_level_info[neighborCellId]==LOCAL_LEVEL)
          {
            // first erase from old level list, assign it a new level then add to new level list
            level_to_cell_map[LOCAL_LEVEL].erase(std::find(level_to_cell_map[LOCAL_LEVEL].begin(),
              level_to_cell_map[LOCAL_LEVEL].end(), neighborCellId));
            cell_level_info[neighborCellId] = level-1;  
            level_to_cell_map[level - 1].push_back(neighborCellId);

            // Now we need to send this cell and its point to remote processes          
            std::vector<vtkIdType> pts_(cell_to_point_map[neighborCellId]);
            int npts_ = pts_.size();
            
            // Currently sending to only one process but we should sent it to all the remote process that its points belong
            // Not sure what would be best here? Earlier cell was sent to the remote of first point
            // Quick fix: Iterate over all points
            if (destProcess!=this->UpdatePiece){

              // send the cell
              cell_partitioninfo.Procs.push_back(destProcess); 
              cell_partitioninfo.GlobalIds.push_back(neighborCellId + this->ZoltanCallbackData.ProcessOffsetsCellId[this->UpdatePiece]);

              // send all the points
              for (int i = 0; i < npts_; ++i)
              {  
                process_vector.push_back( process_tuple(pts_[i], destProcess) );
              }
            }
          }
        }
      }
    
      for (int level = LOCAL_LEVEL; level <= REMOTE_LEVEL ; ++level)
      {
        my_debug("Level:"<<level<<"\t Cells:"<<level_to_cell_map[level].size()<<"\t Cells:"<<std::count(cell_level_info.begin(), cell_level_info.end(), level) );
        
        if (level>GHOST_LEVEL && level<REMOTE_LEVEL){
          for (vtkIdType i=0; i<level_to_cell_map[level].size(); i++) {
            vtkIdType cellId = level_to_cell_map[level][i];
//            if (cell_status[cellId]>2) {
              // I want to keep those points too
              std::vector<vtkIdType> pts_(cell_to_point_map[cellId]);
              int npts_ = pts_.size();
              if (std::find(cell_partitioninfo.LocalIdsToKeep.begin(),cell_partitioninfo.LocalIdsToKeep.end(), cellId)!=cell_partitioninfo.LocalIdsToKeep.end()) {
                my_debug("DUPLICATE");
              }
              cell_partitioninfo.LocalIdsToKeep.push_back(cellId);
  #ifdef VTK_ZOLTAN2_PARTITION_FILTER
              cell_partitioninfo.LocalIdsToSend.push_back(level-GHOST_LEVEL);
  #endif
              
              for (int k = 0; k < npts_; ++k)
              {
                if (localId_to_process_map[pts_[k]]!=this->UpdatePiece) {
                  point_partitioninfo.LocalIdsToKeep.push_back(pts_[k]);
                }
              }
//            }
            
          }
        }

      }
    }
  }
  //
  // remove any duplicated export ids (note that these are pairs, so equality is both of <id,process>
  // some points might be sent to multiple locations, that's allowed.
  //
  std::sort(process_vector.begin(), process_vector.end());
  process_vector.resize(std::unique(process_vector.begin(), process_vector.end()) - process_vector.begin());

  //
  // remove any duplicated ids of points we are keeping as well as sending, not pairs here
  //
  std::sort(point_partitioninfo.LocalIdsToKeep.begin(), point_partitioninfo.LocalIdsToKeep.end());
  point_partitioninfo.LocalIdsToKeep.resize(std::unique(point_partitioninfo.LocalIdsToKeep.begin(), point_partitioninfo.LocalIdsToKeep.end()) - point_partitioninfo.LocalIdsToKeep.begin());
  
//  std::sort(cell_partitioninfo.LocalIdsToKeep.begin(), cell_partitioninfo.LocalIdsToKeep.end());
//  cell_partitioninfo.LocalIdsToKeep.resize(std::unique(cell_partitioninfo.LocalIdsToKeep.begin(), cell_partitioninfo.LocalIdsToKeep.end()) - cell_partitioninfo.LocalIdsToKeep.begin());

  //
  // After examining cells, we found some points we need to send away,
  // we must add these to the points the original load balance already declared in export lists 
  //
  point_partitioninfo.GlobalIds.reserve(loadBalanceData.numExport + process_vector.size());
  point_partitioninfo.Procs.reserve(loadBalanceData.numExport + process_vector.size());
  
  // 1) add the points from original zoltan load balance to send list
  for (int i=0; i<loadBalanceData.numExport; ++i) {
    point_partitioninfo.GlobalIds.push_back(loadBalanceData.exportGlobalGids[i]);
    point_partitioninfo.Procs.push_back(loadBalanceData.exportProcs[i]);
  }

  // 2) add the points from cell tests just performed to send list
  for (std::vector<process_tuple>::iterator x=process_vector.begin(); x!=process_vector.end(); ++x) 
  {
    point_partitioninfo.GlobalIds.push_back(x->first + this->ZoltanCallbackData.ProcessOffsetsPointId[this->UpdatePiece]);
    point_partitioninfo.Procs.push_back(x->second);
  }
  
  my_debug("Ghost points generated: LOCAL GHOST:"<<cell_partitioninfo.LocalIdsToKeep.size()<<"\t REMOTE GHOST+ALL:"<<cell_partitioninfo.GlobalIds.size());
  
  this->Controller->Barrier();

  vtkDebugMacro(<<"BuildCellToProcessList "  << 
    " numImport : " << this->LoadBalanceData.numImport <<
    " numExport : " << point_partitioninfo.GlobalIds .size()
  );
}
//----------------------------------------------------------------------------
