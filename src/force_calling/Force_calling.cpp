/*
 * Force_calling.cpp
 *
 *  Created on: Aug 24, 2017
 *      Author: sedlazec
 */

#include "Force_calling.h"

char assign_type(short type) {

	switch (type) {
	case 0: //DEL
		return DEL;
	case 1: //DUP
		return INV;
	case 2: //INV
		return INV;
	case 3: //TRA
		return TRA;
	case 4: //INS
		return INS;
	case 6:
		return NEST;
	}
	return ' '; //TODO check default. Should not happen!
}
void fill_tree(IntervallTree & final, TNode *& root_final, RefVector ref, std::map<std::string, long>& ref_lens) {
	//prepare lookup:

	long length = 0;
	for (size_t i = 0; i < ref.size(); i++) {
		ref_lens[ref[i].RefName.c_str()] = length;
		length += ref[i].RefLength + Parameter::Instance()->max_dist;
	}

	//parse VCF file
	std::vector<strvcfentry> entries = parse_vcf(Parameter::Instance()->input_vcf, 0);
	std::cout << "\t\t" << entries.size() << " SVs found in input." << std::endl;
	for (size_t i = 0; i < entries.size(); i++) {
		if (entries[i].type != -1) {
			position_str svs;
			svs.start.min_pos = (long) entries[i].start.pos + ref_lens[entries[i].start.chr];
			svs.stop.max_pos = (long) entries[i].stop.pos + ref_lens[entries[i].stop.chr];
			read_str read;
			read.SV = assign_type(entries[i].type);
			read.strand=entries[i].strands;
			read.type = 2; //called
			svs.support["input"] = read;
			Breakpoint * br = new Breakpoint(svs, (long) entries[i].sv_len);
			final.insert(br, root_final);
			//std::cout << "Print:" << std::endl;
			//final.print(root_final);
		} else {
			cerr << "Invalid type found skipping" << endl;
		}
	}
	entries.clear();
}

void force_calling(std::string bam_file, IPrinter *& printer) {
	cout<<"Force calling SVs"<<endl;
	//parse reads
	//only process reads overlapping SV
	estimate_parameters(Parameter::Instance()->bam_files[0]);
	BamParser * mapped_file = 0;
	RefVector ref;
	std::string read_filename = Parameter::Instance()->bam_files[0];
	if (read_filename.find("bam") != string::npos) {
		mapped_file = new BamParser(read_filename);
		ref = mapped_file->get_refInfo();
	} else {
		cerr << "File Format not recognized. File must be a sorted .bam file!" << endl;
		exit(0);
	}
	std::cout << "Construct Tree..." << std::endl;

	//construct the tree:
	IntervallTree final;
	TNode * root_final = NULL;
	std::map<std::string, long> ref_lens;
	fill_tree(final, root_final, ref, ref_lens);

	std::cout << "Start parsing..." << std::endl;

	int current_RefID = 0;

	//FILE * alt_allel_reads;
	FILE * ref_allel_reads;
	if (Parameter::Instance()->genotype) {
		std::string output = Parameter::Instance()->tmp_file.c_str();
		output += "ref_allele";
		ref_allel_reads = fopen(output.c_str(), "wb");
	}
	Alignment * tmp_aln = mapped_file->parseRead(Parameter::Instance()->min_mq);
	long ref_space = ref_lens[ref[tmp_aln->getRefID()].RefName];
	long num_reads = 0;
	while (!tmp_aln->getQueryBases().empty()) {
		if ((tmp_aln->getAlignment()->IsPrimaryAlignment()) && (!(tmp_aln->getAlignment()->AlignmentFlag & 0x800) && tmp_aln->get_is_save())) {
			//change CHR:
			if (current_RefID != tmp_aln->getRefID()) {
				current_RefID = tmp_aln->getRefID();
				ref_space = ref_lens[ref[tmp_aln->getRefID()].RefName];
				std::cout << "\tSwitch Chr " << ref[tmp_aln->getRefID()].RefName << std::endl;				//" " << ref[tmp_aln->getRefID()].RefLength
			}

			//check if overlap with any breakpoint!!
			long read_start_pos = (long) tmp_aln->getPosition() - (long)Parameter::Instance()->max_dist;
			read_start_pos += ref_space;
			long read_stop_pos = read_start_pos + (long) tmp_aln->getAlignment()->Length + (long)Parameter::Instance()->max_dist;	//getRefLength();//(long) tmp_aln->getPosition();

			if (final.overlaps(read_start_pos, read_stop_pos, root_final)) {
				//SCAN read:
				std::vector<str_event> aln_event;
				std::vector<aln_str> split_events;
				if (tmp_aln->getMappingQual() > Parameter::Instance()->min_mq) {
					double score = tmp_aln->get_scrore_ratio();
#pragma omp parallel // starts a new team
					{
#pragma omp sections
						{
							{
								//	clock_t begin = clock();
								if ((score == -1 || score > Parameter::Instance()->score_treshold)) {
									aln_event = tmp_aln->get_events_Aln();
								}
								//	Parameter::Instance()->meassure_time(begin, " Alignment ");
							}
#pragma omp section
							{
								//		clock_t begin_split = clock();
								split_events = tmp_aln->getSA(ref);
								//		Parameter::Instance()->meassure_time(begin_split," Split reads ");
							}
						}
					}
					//tmp_aln->set_supports_SV(aln_event.empty() && split_events.empty());

					//Store reference supporting reads for genotype estimation:
					str_read tmp;
					tmp.SV_support = !(aln_event.empty() && split_events.empty());
					if ((Parameter::Instance()->genotype && !tmp.SV_support) && (score == -1 || score > Parameter::Instance()->score_treshold)) {
						//write read:
						//cout<<"REf: "<<tmp_aln->getName()<<" "<<tmp_aln->getPosition()<<" "<<tmp_aln->getRefLength()<<endl;
						tmp.chr = ref[tmp_aln->getRefID()].RefName;
						tmp.start = tmp_aln->getPosition();
						tmp.length = tmp_aln->getRefLength();
						tmp.SV_support = false;
						fwrite(&tmp, sizeof(struct str_read), 1, ref_allel_reads);
					}

					//store the potential SVs:
					if (!aln_event.empty()) {
						add_events(tmp_aln, aln_event, 0, ref_space, final, root_final, num_reads, true);
					}
					if (!split_events.empty()) {
						add_splits(tmp_aln, split_events, 1, ref, final, root_final, num_reads, true);
					}
				}
			}
		}
		//get next read:
		mapped_file->parseReadFast(Parameter::Instance()->min_mq, tmp_aln);

		num_reads++;

		if (num_reads % 10000 == 0) {
			cout << "\t\t# Processed reads: " << num_reads << endl;
		}
	}

	std::cout << "Print:" << std::endl;
	//final.print(root_final);

	//filter and copy results:
	std::cout << "Finalizing  .." << std::endl;

	if (Parameter::Instance()->genotype) {
		fclose(ref_allel_reads);
	}
	//	sweep->finalyze();

	std::vector<Breakpoint*> points;
	final.get_breakpoints(root_final, points);

	//std::cout<<"fin up"<<std::endl;
	for (size_t i = 0; i < points.size(); i++) {
		points[i]->calc_support();
		points[i]->predict_SV();
		printer->printSV(points[i]); //redo! Ignore min support + STD etc.
	}
}
