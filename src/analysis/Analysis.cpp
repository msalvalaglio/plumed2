/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2012-2015 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.

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

#include "Analysis.h"
#include "core/ActionSet.h"
#include "core/ActionWithValue.h"
#include "core/PlumedMain.h"
#include "core/Atoms.h"
#include "tools/IFile.h"
#include "reference/ReferenceConfiguration.h"
#include "reference/ReferenceArguments.h"
#include "reference/ReferenceAtoms.h"
#include "reference/MetricRegister.h"
#include "reference/FakeFrame.h"
#include "AnalysisWithAnalysableOutput.h"

namespace PLMD {
namespace analysis {

//+PLUMEDOC INTERNAL reweighting
/*
Calculate free energies from a biassed/higher temperature trajectory. 

We can use our knowledge of the Boltzmann distribution in the cannonical ensemble to reweight the data
contained in trajectories.  Using this procedure we can take trajectory at temperature \f$T_1\f$ and use it to 
extract probabilities at a different temperature, \f$T_2\f$, using:

\f[
P(s',t) = \frac{ \sum_{t'}^t \delta( s(x) - s' ) \exp\left( +( \left[\frac{1}{T_1} - \frac{1}{T_2}\right] \frac{U(x,t')}{k_B} \right) }{ \sum_{t'}^t \exp\left( +\left[\frac{1}{T_1} - \frac{1}{T_2}\right] \frac{U(x,t')}{k_B} \right) }
\f]

where \f$U(x,t')\f$ is the potential energy of the system.  Alternatively, if a static or pseudo-static bias \f$V(x,t')\f$ is acting on 
the system we can remove this bias and get the unbiased probability distribution using:

\f[
P(s',t) = \frac{ \sum_{t'}^t \delta( s(x) - s' ) \exp\left( +\frac{V(x,t')}{k_B T} \right) }{ \sum_t'^t \exp\left( +\frac{V(x,t')}{k_B T} \right) } 
\f]

*/
//+ENDPLUMEDOC

void Analysis::registerKeywords( Keywords& keys ){
  Action::registerKeywords( keys );
  ActionPilot::registerKeywords( keys );
  ActionAtomistic::registerKeywords( keys );
  ActionWithArguments::registerKeywords( keys );
  keys.use("ARG"); keys.reset_style("ARG","atoms");
  keys.add("atoms","ATOMS","the atoms whose positions we are tracking for the purpose of analysing the data");
  keys.add("compulsory","METRIC","EUCLIDEAN","how are we measuring the distances between configurations");
  keys.add("compulsory","STRIDE","1","the frequency with which data should be stored for analysis.  This is not required if you specify REUSE_INPUT_DATA_FROM/USE_OUTPUT_DATA_FROM");
  keys.add("compulsory","RUN","the frequency with which to run the analysis algorithm. This is not required if you specify USE_ALL_DATA/REUSE_INPUT_DATA_FROM/USE_OUTPUT_DATA_FROM");
  keys.add("atoms-2","REUSE_INPUT_DATA_FROM","using this form of input allows one to do multiple analyses on the same set of data.  The data "
                                       "from the referenced Analysis action will be re-analysed using this new form of analysis after having "
                                       "been analysed using the form of analysis specified in the action that was previously referenced. The " 
                                       "frequency (STRIDE) of storage of the data and the frequency of running this second form of analysis (RUN) "
                                       "are the same as those in the referenced analysis object");
  keys.add("atoms-3","USE_OUTPUT_DATA_FROM","using this form of input allows one to analyse projections of the data generated using some dimensionality reduction algoirthm. "
                                            "The dimensionality reduction will have been performed using the referenced Analysis action.  If landmark configurations were "
                                            "used projections of all stored points will be generated using the appropriate out of sample technique.  The frequency (STRIDE) "
                                            "of storage of the data and the frequency with which this particular form of analysis are performed (RUN) are the same as those "
                                            "in the referenced analysis object.");
  keys.addFlag("USE_ALL_DATA",false,"use the data from the entire trajectory to perform the analysis");
  keys.add("optional","FMT","the format that should be used in analysis output files");
  keys.addFlag("REWEIGHT_BIAS",false,"reweight the data using all the biases acting on the dynamics. For more information see \\ref reweighting.");
  keys.add("optional","TEMP","the system temperature.  This is required if you are reweighting or doing free energies.");
  keys.add("optional","REWEIGHT_TEMP","reweight data from a trajectory at one temperature and output the probability "
                                      "distribution at a second temperature. For more information see \\ref reweighting. "
                                      "This is not possible during postprocessing.");
  keys.addFlag("WRITE_CHECKPOINT",false,"write out a checkpoint so that the analysis can be restarted in a later run");
  // keys.add("hidden","IGNORE_REWEIGHTING","this allows you to ignore any reweighting factors");
  keys.reserveFlag("NOMEMORY",false,"analyse each block of data separately");
  ActionWithVessel::registerKeywords( keys ); keys.remove("TOL"); 
}

Analysis::Analysis(const ActionOptions&ao):
Action(ao),
ActionPilot(ao),
ActionAtomistic(ao),
ActionWithArguments(ao),
ActionWithVessel(ao),
single_run(false),
nomemory(true),
write_chq(false),
reusing_data(false),
dimred_data(false),
ignore_reweight(false),
dmat_type(unset),
needeng(false),
idata(0),
firstAnalysisDone(false),
old_norm(0.0),
ofmt("%f"),
current_args(getNumberOfArguments()),
argument_names(getNumberOfArguments())
{
  parse("FMT",ofmt);  // Read the format for output files
  // Make a vector containing all the argument names
  for(unsigned i=0;i<getNumberOfArguments();++i) argument_names[i]=getPntrToArgument(i)->getName();
  // Read in the metric style
  parse("METRIC",metricname); std::vector<AtomNumber> atom_numbers;
  ReferenceConfiguration* checkref=metricRegister().create<ReferenceConfiguration>( metricname );
  // Check if we should read atoms
  ReferenceAtoms* hasatoms=dynamic_cast<ReferenceAtoms*>( checkref );
  if( hasatoms ){
      parseAtomList("ATOMS",atom_numbers); requestAtoms(atom_numbers);
      log.printf("  monitoring positions of atoms ");
      for(unsigned i=0;i<atom_numbers.size();++i) log.printf("%d ",atom_numbers[i].serial() );
      log.printf("\n");
  }
  // Check if we should read arguments
  ReferenceArguments* hasargs=dynamic_cast<ReferenceArguments*>( checkref );
  if( !hasargs && getNumberOfArguments()!=0 ) error("use of arguments with metric type " + metricname + " is invalid");
  if( hasatoms && hasargs ) error("currently dependencies break if you have both arguments and atoms");
  // And delte the fake reference we created
  delete checkref;

  std::string prev_analysis; parse("REUSE_INPUT_DATA_FROM",prev_analysis);
  if( prev_analysis.length()>0 ){
      reusing_data=true;
      mydatastash=plumed.getActionSet().selectWithLabel<Analysis*>( prev_analysis );
      if( !mydatastash ) error("could not find analysis action named " + prev_analysis );
      // parseFlag("IGNORE_REWEIGHTING",ignore_reweight);
      // if( ignore_reweight ) log.printf("  reusing data stored by %s but ignoring all reweighting\n",prev_analysis.c_str() );
      else log.printf("  reusing data stored by %s\n",prev_analysis.c_str() ); 
      single_run=mydatastash->single_run; freq=mydatastash->freq;
  } else {
      std::string prev_dimred; parse("USE_OUTPUT_DATA_FROM",prev_dimred);
      if( prev_dimred.length()>0 ){
          reusing_data=true; dimred_data=true;
          dimredstash=plumed.getActionSet().selectWithLabel<AnalysisWithAnalysableOutput*>( prev_dimred );
          if( !dimredstash ) error("could not find analysis output action named " + prev_dimred );
          mydatastash=dynamic_cast<Analysis*>( dimredstash ); plumed_assert( mydatastash );
          if( ignore_reweight ) log.printf("  using projections calculated by %s but ignoring all reweighting\n",prev_dimred.c_str() );
          else log.printf("  using projections calculated by %s\n",prev_dimred.c_str() ); 
          single_run=mydatastash->single_run; freq=mydatastash->freq;
      } else { 
          if( keywords.exists("REWEIGHT_BIAS") ){
             bool dobias; parseFlag("REWEIGHT_BIAS",dobias);
             if( dobias ){
                 std::vector<ActionWithValue*> all=plumed.getActionSet().select<ActionWithValue*>();
                 if( all.empty() ) error("your input file is not telling plumed to calculate anything");
                 std::vector<Value*> arg( getArguments() );
                 log.printf("  reweigting using the following biases ");
                 for(unsigned j=0;j<all.size();j++){
                     std::string flab; flab=all[j]->getLabel() + ".bias";
                     if( all[j]->exists(flab) ){ 
                        biases.push_back( all[j]->copyOutput(flab) ); 
                        arg.push_back( all[j]->copyOutput(flab) ); 
                        log.printf(" %s",flab.c_str()); 
                     }
                 }
                 log.printf("\n");
                 if( biases.empty() ) error("you are asking to reweight bias but there does not appear to be a bias acting on your system");
                 requestArguments( arg ); 
             }
          }
      }

      rtemp=0;      
      if( keywords.exists("REWEIGHT_TEMP") ) parse("REWEIGHT_TEMP",rtemp);
      if( rtemp!=0 ){
         needeng=true;
         log.printf("  reweighting simulation to probabilities at temperature %f\n",rtemp);
         rtemp*=plumed.getAtoms().getKBoltzmann(); 
      } 
      simtemp=0.; parse("TEMP",simtemp);
      if(simtemp>0) simtemp*=plumed.getAtoms().getKBoltzmann();
      else simtemp=plumed.getAtoms().getKbT();

      if( rtemp>0 || !biases.empty() ){
         if(simtemp==0) error("The MD engine does not pass the temperature to plumed so you have to specify it using TEMP");
      }

      parseFlag("USE_ALL_DATA",single_run); 
      if( !single_run && !reusing_data ){
          parse("RUN",freq );
          log.printf("  running analysis every %u steps\n",freq);
          if( freq%getStride()!= 0 ) error("Frequncy of running is not a multiple of the stride");
          ndata=freq/getStride();
          data.resize( ndata );
          for(unsigned i=0;i<ndata;++i){ 
             data[i]=metricRegister().create<ReferenceConfiguration>( metricname ); 
             data[i]->setNamesAndAtomNumbers( atom_numbers, argument_names );
          }
          logweights.resize( ndata );
      } else if( reusing_data ){
          single_run=mydatastash->single_run;
          if( !single_run ){
              freq=mydatastash->freq;
              ndata=freq/mydatastash->getStride();
              setStride( freq );
          }
      } else if( single_run ) {       
          log.printf("  analyzing all data in trajectory\n");
      }
      if( keywords.exists("NOMEMORY") ){ nomemory=false; parseFlag("NOMEMORY",nomemory); }
      if(nomemory) log.printf("  doing a separate analysis for each block of data\n");
      parseFlag("WRITE_CHECKPOINT",write_chq);
      if( write_chq && single_run ){
          write_chq=false;
          warning("ignoring WRITE_CHECKPOINT flag because we are analyzing all data");
      }

      // We need no restart file if we are just collecting data and analyzing all of it
      std::string filename = getName() + "_" + getLabel() + ".chkpnt"; 
      if( write_chq ) rfile.link(*this);
      if( getRestart() ){
          if( single_run ) error("cannot restart histogram when using the USE_ALL_DATA option");
          if( !write_chq ) warning("restarting without writing a checkpoint file is somewhat strange");
          // Read in data from input file
          readDataFromFile( filename );
          // Setup the restart file (append mode)
          if( write_chq ) rfile.open( filename.c_str() );  // In append mode automatically because of restart
          // Run the analysis if we stoped in the middle of it last time
          log.printf("  restarting analysis with %u points read from restart file\n",idata);
      } else if( write_chq ){
          // Setup the restart file (delete any old one)
          rfile.open( filename.c_str() );  // In overwrite mode automatically because there is no restart
      }
      if( write_chq ){
         rfile.addConstantField("old_normalization");
         for(unsigned i=0;i<getNumberOfArguments();++i) rfile.setupPrintValue( getPntrToArgument(i) );
      }
  }
}

void Analysis::readDataFromFile( const std::string& filename ){
  FILE* fp=fopen(filename.c_str(),"r"); double tstep, oldtstep; 
  if(fp!=NULL){
     bool do_read=true, first=true;
     while (do_read) {
        PDB mypdb;
        do_read=mypdb.readFromFilepointer(fp,plumed.getAtoms().usingNaturalUnits(),0.1/atoms.getUnits().getLength());
        if(do_read){
           data[idata]->set( mypdb );
           data[idata]->parse("TIME",tstep);
           if( !first && ((tstep-oldtstep) - getStride()*plumed.getAtoms().getTimeStep())>plumed.getAtoms().getTimeStep() ){
              error("frequency of data storage in " + filename + " is not equal to frequency of data storage plumed.dat file");
           }
           data[idata]->parse("LOG_WEIGHT",logweights[idata]);
           data[idata]->parse("OLD_NORM",old_norm);
           data[idata]->checkRead();
           idata++; first=false; oldtstep=tstep;
        } else{
           break; 
        } 
     }
    fclose(fp);
  }
  if(old_norm>0) firstAnalysisDone=true;
}

void Analysis::parseOutputFile( const std::string& key, std::string& filename ){
  parse(key,filename);
  if(filename=="dont output") return;

  if( !getRestart() ){
      OFile ofile; ofile.link(*this);
      ofile.setBackupString("analysis");
      ofile.backupAllFiles(filename);
  } 
}

void Analysis::prepare(){
  if(needeng) plumed.getAtoms().setCollectEnergy(true);
}

void Analysis::calculate(){
// do nothing
}

void Analysis::accumulate(){
  // Don't store the first step (also don't store if we are getting data from elsewhere)
  if( (!single_run && getStep()==0) || reusing_data ) return;
  // This is used when we have a full quota of data from the first run
  if( !single_run && idata==logweights.size() ) return; 

  // Retrieve the bias
  double bias=0.0; for(unsigned i=0;i<biases.size();++i) bias+=biases[i]->get();

  double ww=0;
  if(needeng){
     double energy=plumed.getAtoms().getEnergy()+bias;
     // Reweighting because of temperature difference
     ww=-( (1.0/rtemp) - (1.0/simtemp) )*(energy+bias);
  }
  // Reweighting because of biases
  if( !biases.empty() ) ww += bias/ simtemp;

  // Get the arguments ready to transfer to reference configuration
  for(unsigned i=0;i<getNumberOfArguments();++i) current_args[i]=getArgument(i);

  if(single_run){
     data.push_back( metricRegister().create<ReferenceConfiguration>( metricname ) );
     plumed_dbg_assert( data.size()==idata+1 );
     data[idata]->setNamesAndAtomNumbers( getAbsoluteIndexes(), argument_names );
     data[idata]->setReferenceConfig( getPositions(), current_args, getMetric() );
     logweights.push_back(ww);
  } else {
     // Get the arguments and store them in a vector of vectors
     data[idata]->setReferenceConfig( getPositions(), current_args, getMetric() );
     logweights[idata] = ww; 
  }

  // Write data to checkpoint file
  if( write_chq ){
     rfile.rewind();
     if( plumed.getAtoms().usingNaturalUnits() ) data[idata]->print( 1.0, rfile, getTime(), logweights[idata], old_norm );
     else data[idata]->print( atoms.getUnits().getLength()/0.1, rfile, getTime(), logweights[idata], old_norm );
     rfile.flush();
  }
  // Increment data counter
  idata++;
}

Analysis::~Analysis(){
  for(unsigned i=0;i<data.size();++i ) delete data[i];
  if( write_chq ) rfile.close();
}

std::vector<double> Analysis::getMetric() const {
  // Add more exotic metrics in here -- FlexibleHill for instance
  std::vector<double> empty;
  if( metricname=="EUCLIDEAN" ){
      empty.resize( getNumberOfArguments(), 1.0 );
  } else if( metricname=="DOTPRODUCT" ){
      empty.resize( getNumberOfArguments(), 1.0 );
  }
  return empty;
}

double Analysis::getWeight( const unsigned& idata ) const {
  if( !reusing_data ){
     plumed_dbg_assert( idata<data.size() );
     return data[idata]->getWeight();
  } else if( dimred_data ){
     return dimredstash->getOutputWeight(idata);
  } else {
     return mydatastash->getWeight(idata);
  }
}

ReferenceConfiguration* Analysis::getReferenceConfiguration( const unsigned& idata ){
  if( !reusing_data ){
      return data[idata];
  } else if( dimred_data ){
      // ReferenceConfigurationOptions("EUCLIDEAN");
      // ReferenceConfiguration* mydata=metricRegister().create<ReferenceConfiguration>("EUCLIDEAN");
      // unsigned ndim=dimredstash->getDimensionOfOutputPoints();
      // std::vector<std::string> dimnames(ndim); dimredstash->getPropertyNames( dimnames );
      // mydata->setNamesAndAtomNumbers( std::vector<AtomNumber>(), dimnames );
      // std::vector<double> pp(ndim); dimredstash->getOutputForPoint( idata, pp );
      // std::vector<double> empty( pp.size() );
      // mydata->setReferenceConfig( std::vector<Vector>(), pp, empty );  
      return dimredstash->getOutputConfiguration( idata );
  } else {
      return mydatastash->getReferenceConfiguration( idata );
  }
}

void Analysis::finalizeWeights( const bool& ignore_weights ){
  // Check that we have the correct ammount of data
  if( !reusing_data && idata!=logweights.size() ) error("something has gone wrong.  Am trying to run analysis but I don't have sufficient data");

  norm=0;  // Reset normalization constant
  if( ignore_weights ){
      for(unsigned i=0;i<logweights.size();++i){
          data[i]->setWeight(1.0); norm+=1.0;
      } 
  } else if( nomemory ){
      // Find the maximum weight
      double maxweight=logweights[0];
      for(unsigned i=1;i<getNumberOfDataPoints();++i){
         if(logweights[i]>maxweight) maxweight=logweights[i];
      }
      // Calculate normalization constant
      for(unsigned i=0;i<logweights.size();++i){
         norm+=exp( logweights[i]-maxweight );
      }
      // Calculate weights (no memory)
      for(unsigned i=0;i<logweights.size();++i) data[i]->setWeight( exp( logweights[i]-maxweight ) );
  // Calculate normalized weights (with memory)
  } else {
      // Calculate normalization constant
      for(unsigned i=0;i<logweights.size();++i){
         norm+=exp( logweights[i] );
      }
      if( !firstAnalysisDone ) old_norm=1.0;
      // Calculate weights (with memory)
      for(unsigned i=0;i<logweights.size();++i) data[i]->setWeight( exp( logweights[i] ) / old_norm );
      if( !firstAnalysisDone ) old_norm=0.0;
  }
  
}

void Analysis::getDataPoint( const unsigned& idata, std::vector<double>& point, double& weight ) const {
  plumed_dbg_assert( getNumberOfAtoms()==0 );
  if( !reusing_data ){
      plumed_assert( dmat_type==unset );
      plumed_dbg_assert( idata<logweights.size() &&  point.size()==getNumberOfArguments() );
      for(unsigned i=0;i<point.size();++i) point[i]=data[idata]->getReferenceArgument(i);
      weight=data[idata]->getWeight();
  } else if( dimred_data ){
      weight = getWeight( idata );
      dimredstash->getOutputForPoint( idata, point );
  } else {
      mydatastash->getDataPoint( idata, point, weight );
  }
}

double Analysis::getDistanceBetweenFrames( const unsigned& iframe, const unsigned& jframe, const bool& squared ){
  if( reusing_data ) return mydatastash->getDistanceBetweenFrames( iframe, jframe, squared );
  if( dmat_type==unset ) return distance( getPbc(), getArguments(), getReferenceConfiguration(iframe), getReferenceConfiguration(jframe), squared );
  if( dmat_type==squared && !squared ) return sqrt( pairwise_dissimilarity_matrix(iframe,jframe) ); 
  if( dmat_type==dist && squared ) return pairwise_dissimilarity_matrix(iframe,jframe)*pairwise_dissimilarity_matrix(iframe,jframe);
  return pairwise_dissimilarity_matrix(iframe,jframe);
}

void Analysis::runAnalysis(){
  // Note : could add multiple walkers here - simply read in the data from all
  // other walkers here if we are writing the check points.

  // Calculate the final weights from the log weights 
  if( !reusing_data ){ 
     finalizeWeights( ignore_reweight ); 
  } else {
     // mydatastash->finalizeWeights( ignore_reweight ); Weights will have been finalized by previous analysis
     norm=mydatastash->retrieveNorm();
  }
  // And run the analysis
  performAnalysis(); idata=0;
  // Update total normalization constant
  old_norm+=norm; firstAnalysisDone=true;

}

double Analysis::getNormalization() const {
  if( nomemory || !firstAnalysisDone ) return norm;
  return ( 1. + norm/old_norm );
}

double Analysis::getTemp() const {
  return simtemp;
}

void Analysis::update(){
  accumulate();
  if( !single_run ){
    if( getStep()>0 && getStep()%freq==0 ) runAnalysis(); 
    else if( !reusing_data && idata==logweights.size() ) error("something has gone wrong. Probably a wrong initial time on restart"); 
  }
}

bool Analysis::getPeriodicityInformation(const unsigned& i, std::string& dmin, std::string& dmax){
  bool isperiodic=getPntrToArgument(i)->isPeriodic();
  if(isperiodic) getPntrToArgument(i)->getDomain(dmin,dmax);
  return isperiodic;
}

void Analysis::runFinalJobs() {
  if( !single_run ) return;
  if( getNumberOfDataPoints()==0 ) error("no data is available for analysis");
  runAnalysis(); 
}

unsigned Analysis::getNumberOfDataPoints() const {
  if( !reusing_data ){
     plumed_dbg_assert( data.size()==logweights.size() );
     return data.size();
  } else if( dimred_data ){
     return dimredstash->getNumberOfOutputPoints();
  } else {
     return mydatastash->getNumberOfDataPoints();
  }
}

unsigned Analysis::getRunFrequency() const {
  return freq;
}

}
}
