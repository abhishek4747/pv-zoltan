/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkBoundsExtentTranslator.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkBoundsExtentTranslator.h"
#include "vtkObjectFactory.h"
#include "vtkMultiProcessController.h"
#include "vtkMath.h"
#include "vtkBoundingBox.h"
#include "vtkPKdTree.h"

vtkStandardNewMacro(vtkBoundsExtentTranslator);

//----------------------------------------------------------------------------
vtkBoundsExtentTranslator::vtkBoundsExtentTranslator()
{
  this->MaximumGhostDistance = 0;
  this->BoundsHalosPresent = 0;
  this->KdTree = NULL;
}

//----------------------------------------------------------------------------
vtkBoundsExtentTranslator::~vtkBoundsExtentTranslator()
{
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  if(!this->BoundsTable.empty())
    {
    vtkIndent nextIndent = indent.GetNextIndent();
    double* bounds = &this->BoundsTable[0];
    int i;
    
    os << indent << "BoundsTable: 0: "
       << bounds[0] << " " << bounds[1] << " "
       << bounds[2] << " " << bounds[3] << " "
       << bounds[4] << " " << bounds[5] << "\n";
    for(i=1; i<this->GetNumberOfPieces();++i)
      {
      bounds += 6;
      os << nextIndent << "             " << i << ": "
         << bounds[0] << " " << bounds[1] << " "
         << bounds[2] << " " << bounds[3] << " "
         << bounds[4] << " " << bounds[5] << "\n";
      }
    }
  else
    {
    os << indent << "BoundsTable: (none)\n";
    }
  os << indent << "MaximumGhostDistance: " << this->MaximumGhostDistance << "\n";
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetNumberOfPieces(int pieces)
{
  // Allocate a table for this number of pieces.
  this->BoundsTable.resize(pieces*6);
  this->BoundsTableHalo.resize(pieces*6);
  this->Superclass::SetNumberOfPieces(pieces);
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::ExchangeBoundsForAllProcesses(vtkMultiProcessController *controller, double localbounds[6])
{
  // init bounds storage arrays
  int piece = controller->GetLocalProcessId();
  this->SetNumberOfPieces(controller->GetNumberOfProcesses());
  //
  controller->AllGather(localbounds, &this->BoundsTable[0], 6);
  //
  for (int i=0; i<controller->GetNumberOfProcesses(); i++) {
    this->SetBoundsForPiece(i, &this->BoundsTable[i*6]);
  }
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetBoundsForPiece(int piece, double* bounds)
{
  if ((piece*6)>this->BoundsTable.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return;
    }
  memcpy(&this->BoundsTable[piece*6], bounds, sizeof(double)*6);
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetBoundsForPiece(int piece, vtkBoundingBox &box)
{
  double bounds[6];
  box.GetBounds(bounds);
  this->SetBoundsForPiece(piece, bounds);
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetBoundsHaloForPiece(int piece, double* bounds)
{
  if ((piece*6)>this->BoundsTableHalo.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return;
    }
  memcpy(&this->BoundsTableHalo[piece*6], bounds, sizeof(double)*6);
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetBoundsHaloForPiece(int piece, vtkBoundingBox &box)
{
  double bounds[6];
  box.GetBounds(bounds);
  this->SetBoundsHaloForPiece(piece, bounds);
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::InitWholeBounds()
{
  vtkBoundingBox wholeBounds(&this->BoundsTable[0]);
  for (int p=1; p<this->GetNumberOfPieces(); p++) {
    wholeBounds.AddBounds(&this->BoundsTable[p*6]);
  }
  wholeBounds.GetBounds(WholeBounds);
}

//----------------------------------------------------------------------------
double *vtkBoundsExtentTranslator::GetWholeBounds()
{
  return this->WholeBounds;
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::GetBoundsForPiece(int piece, double* bounds)
{
  if ((piece*6)>this->BoundsTable.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return;
    }
  memcpy(bounds, &this->BoundsTable[piece*6], sizeof(double)*6);
}

//----------------------------------------------------------------------------
double* vtkBoundsExtentTranslator::GetBoundsForPiece(int piece)
{
  static double emptyBounds[6] = {0,-1,0,-1,0,-1};
  if ((piece*6)>this->BoundsTable.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return emptyBounds;
    }
  return &this->BoundsTable[piece*6];
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::GetBoundsHaloForPiece(int piece, double* bounds)
{
  if ((piece*6)>this->BoundsTableHalo.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return;
    }
  memcpy(bounds, &this->BoundsTableHalo[piece*6], sizeof(double)*6);
}

//----------------------------------------------------------------------------
double* vtkBoundsExtentTranslator::GetBoundsHaloForPiece(int piece)
{
  static double emptyBounds[6] = {0,-1,0,-1,0,-1};
  if ((piece*6)>this->BoundsTableHalo.size() || (piece < 0))
    {
    vtkErrorMacro("Piece " << piece << " does not exist.  "
                  "GetNumberOfPieces() is " << this->GetNumberOfPieces());
    return emptyBounds;
    }
  return &this->BoundsTableHalo[piece*6];
}

//----------------------------------------------------------------------------
void vtkBoundsExtentTranslator::SetKdTree(vtkPKdTree *cuts)
{
  this->KdTree = cuts;
}

//----------------------------------------------------------------------------
vtkPKdTree *vtkBoundsExtentTranslator::GetKdTree()
{
  return this->KdTree;
}

//----------------------------------------------------------------------------
// Make sure these inherited methods report an error is anyone is calling them
//----------------------------------------------------------------------------
int vtkBoundsExtentTranslator::PieceToExtentByPoints()
{
  vtkErrorMacro("PieceToExtentByPoints not supported.");
  return 0;
}

//----------------------------------------------------------------------------
int vtkBoundsExtentTranslator::PieceToExtent()
{
  return 
    this->PieceToExtentThreadSafe(this->Piece, this->NumberOfPieces,
                                  this->GhostLevel, this->WholeExtent,
                                  this->Extent, this->SplitMode, 0);
}

//----------------------------------------------------------------------------
#define BOUNDSEXTENT_ISAXPY(a,s,l,y,z) \
  a[0] = s[0]>0 ? (z[0] - y[0])/s[0] : 0; \
  a[1] = s[1]>0 ? (z[1] - y[1])/s[1] : 0; \
  a[2] = s[2]>0 ? (z[2] - y[2])/s[2] : 0;
//----------------------------------------------------------------------------
int vtkBoundsExtentTranslator::BoundsToExtentThreadSafe(
  double *bounds, int *wholeExtent, int *resultExtent)
{
  double scaling[3], lengths[3];
  vtkBoundingBox wholebounds(this->GetWholeBounds());
  vtkBoundingBox piecebounds(bounds);
  wholebounds.GetLengths(lengths);
  const double *origin = wholebounds.GetMinPoint();
  double dims[3] = {
    wholeExtent[1]-wholeExtent[0], 
    wholeExtent[3]-wholeExtent[2], 
    wholeExtent[5]-wholeExtent[4], 
  };

  for (int i=0; i<3; i++) {
    scaling[i] = (dims[i]>0.0) ? lengths[i]/dims[i] : 0.0;
  }

  const double *minpt = piecebounds.GetMinPoint();
  const double *maxpt = piecebounds.GetMaxPoint();

  for (int i=0; i<3; i++) {
    resultExtent[i*2+0] = scaling[i]>0 ? static_cast<int>(0.5+(minpt[i]-origin[i])/scaling[i]) : 0;
    resultExtent[i*2+1] = scaling[i]>0 ? static_cast<int>(0.5+(maxpt[i]-origin[i])/scaling[i]) : 0;
  }
  return 1;
}
//----------------------------------------------------------------------------
int vtkBoundsExtentTranslator::PieceToExtentThreadSafe(
  int piece, int numPieces, 
  int ghostLevel, int *wholeExtent, 
  int *resultExtent, int splitMode, 
  int byPoints)
{
  return this->BoundsToExtentThreadSafe(this->GetBoundsForPiece(piece), wholeExtent, resultExtent);
}


