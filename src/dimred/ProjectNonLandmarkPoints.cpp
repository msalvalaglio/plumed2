/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2012 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.0.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "core/ActionRegister.h"
#include "core/PlumedMain.h"
#include "core/ActionSet.h"
#include "tools/Random.h"
#include "reference/MetricRegister.h"
#include "tools/ConjugateGradient.h"
#include "analysis/AnalysisBase.h"
#include "DimensionalityReductionBase.h"

namespace PLMD {
namespace dimred {

class ProjectNonLandmarkPoints : public analysis::AnalysisBase {
private:
/// Tolerance for conjugate gradient algorithm
  double cgtol;
/// Number of diemsions in low dimensional space
  unsigned nlow;
/// We create a reference configuration here so that we can pass projection data
/// quickly
  ReferenceConfiguration* myref; 
/// The class that calcualtes the projection of the data that is required
  DimensionalityReductionBase* mybase;
/// Generate a projection of the ith data point - this is called in two routine
  void generateProjection( const unsigned& idata, std::vector<double>& point );
public:
  static void registerKeywords( Keywords& keys );
  ProjectNonLandmarkPoints( const ActionOptions& ao );
  ~ProjectNonLandmarkPoints();
/// Get the ith data point (this returns the projection)
  void getDataPoint( const unsigned& idata, std::vector<double>& point );
/// Get a reference configuration (this returns the projection)
  ReferenceConfiguration* getReferenceConfiguration( const unsigned& idata );
/// This does nothing -- projections are calculated when getDataPoint and getReferenceConfiguration are called
  void performAnalysis(){}
/// This just calls calculate stress in the underlying projection object
  double calculateStress( const std::vector<double>& pp, std::vector<double>& der );
/// Overwrite virtual function in ActionWithVessel
  void performTask(){ plumed_error(); }  
};

PLUMED_REGISTER_ACTION(ProjectNonLandmarkPoints,"PROJECT_ALL_ANALYSIS_DATA")

void ProjectNonLandmarkPoints::registerKeywords( Keywords& keys ){
  analysis::AnalysisBase::registerKeywords( keys );
  keys.add("compulsory","PROJECTION","the projection that you wish to generate out-of-sample projections with");
  keys.add("compulsory","CGTOL","1E-6","the tolerance for the conjugate gradient optimisation");
}

ProjectNonLandmarkPoints::ProjectNonLandmarkPoints( const ActionOptions& ao ):
Action(ao),
analysis::AnalysisBase(ao),
mybase(NULL)
{
  std::string myproj; parse("PROJECTION",myproj);
  mybase = plumed.getActionSet().selectWithLabel<DimensionalityReductionBase*>( myproj );
  if( !mybase ) error("could not find projection of data named " + myproj ); 
  nlow = mybase->nlow;

  if( mybase->getBaseDataLabel()!=getBaseDataLabel() ) error("mismatch between base data labels"); 

  log.printf("  generating out-of-sample projections using projection with label %s \n",myproj.c_str() );
  parse("CGTOL",cgtol);

  ReferenceConfigurationOptions("EUCLIDEAN");
  myref=metricRegister().create<ReferenceConfiguration>("EUCLIDEAN");
  std::vector<std::string> dimnames(nlow); std::string num;
  for(unsigned i=0;i<nlow;++i){ Tools::convert(i+1,num); dimnames[i] = getLabel() + "." + num; }
  myref->setNamesAndAtomNumbers( std::vector<AtomNumber>(), dimnames );
}

ProjectNonLandmarkPoints::~ProjectNonLandmarkPoints(){
  delete myref;
}

void ProjectNonLandmarkPoints::generateProjection( const unsigned& idata, std::vector<double>& point ){
  ConjugateGradient<ProjectNonLandmarkPoints> myminimiser( this );
  unsigned closest=0; double mindist = sqrt( getDissimilarity( idata, mybase->getDataPointIndexInBase(0) ) );
  mybase->setTargetDistance( 0, mindist );
  for(unsigned i=1;i<mybase->getNumberOfDataPoints();++i){
      double dist = sqrt( getDissimilarity( idata, mybase->getDataPointIndexInBase(i) ) );
      mybase->setTargetDistance( i, dist );
      if( dist<mindist ){ mindist=dist; closest=i; }
  }
  // Put the initial guess near to the closest landmark  -- may wish to use grid here again Sandip??
  Random random; random.setSeed(-1234);
  for(unsigned j=0;j<nlow;++j) point[j]=mybase->projections(closest,j) + (random.RandU01() - 0.5)*0.01;
  myminimiser.minimise( cgtol, point, &ProjectNonLandmarkPoints::calculateStress );
}

ReferenceConfiguration* ProjectNonLandmarkPoints::getReferenceConfiguration( const unsigned& idata ){
  std::vector<double> pp(nlow); std::vector<double> empty( pp.size() ); generateProjection( idata, pp );
  myref->setReferenceConfig( std::vector<Vector>(), pp, empty );
  return myref;
}

void ProjectNonLandmarkPoints::getDataPoint( const unsigned& idata, std::vector<double>& point ){
  if( point.size()!=nlow ) point.resize( nlow );
  generateProjection( idata, point );
}

double ProjectNonLandmarkPoints::calculateStress( const std::vector<double>& pp, std::vector<double>& der ){
  return mybase->calculateStress( pp, der );
} 

}
}
