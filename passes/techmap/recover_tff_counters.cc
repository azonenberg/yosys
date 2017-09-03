/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2017  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/modtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

ModIndex::PortInfo GetDriverOfPort(Cell* cell, RTLIL::IdString port, RTLIL::IdString driveport, ModIndex& index)
{
	auto anchor_t = index.sigmap(cell->getPort(port));
	pool<ModIndex::PortInfo> ports = index.query_ports(anchor_t);
	for(auto x : ports)
	{
		if(x.cell == cell)
			continue;
		if(x.port != driveport)
			continue;
		return x;
	}

	return ModIndex::PortInfo();
}

void recover_tff_counters_worker(
	ModIndex& index,
	Cell *cell,
	unsigned int& /*total_counters*/,
	pool<Cell*>& /*cells_to_remove*/)
{
	SigMap& sigmap = index.sigmap;

	//Start by looking at TFFs
	if (cell->type.str().find("__TFF_") == string::npos)
		return;

	//Find the driver of our T input. It should be a ANDNOT for all but the LSB, which is a NOR.
	auto tdriver = GetDriverOfPort(cell, "\\T", "\\Y", index);
	if(tdriver == ModIndex::PortInfo())
		return;
	if(tdriver.cell->type != "$_NOR_")
		return;

	log("Found candidate counter anchored at %s (%s)\n", log_id(cell->name), cell->type.c_str());

	//Look down the counter toward the MSB.
	//We should see a chain of ANDNOTs and TFFs (TFFs with the same reset polarity as us)
	auto anchor_reset = index.sigmap(cell->getPort("\\R"));
	auto anchor_clock = index.sigmap(cell->getPort("\\C"));
	Cell* current_cell = cell;
	std::vector<Cell*> downstream_flipflops;
	while(true)
	{
		if(current_cell != cell)
			downstream_flipflops.push_back(current_cell);

		//Stage 1: Look for the ANDNOT
		auto q = index.sigmap(current_cell->getPort("\\Q"));
		pool<ModIndex::PortInfo> qports = index.query_ports(q);
		std::vector<Cell*> andnots;
		for(auto x : qports)
		{
			//Skip anything that isn't a ANDNOT
			if(x.cell == current_cell)
				continue;
			if(x.cell->type != "$_ANDNOT_")
				continue;

			//The ANDNOT's port must be A or B (not Y)
			//Y should never happen (multiple drivers) but check just in case
			if(x.port == "\\Y")
				continue;
			andnots.push_back(x.cell);
		}

		//If no ANDNOTs found, we're done - end the chain
		if(andnots.empty())
			break;

		//Stage 2: We found one or more ANDNOTs. Find the first one that has a TFF at its output.
		//TODO: look for longest chain or something?
		bool hit = false;
		for(auto anot : andnots)
		{
			auto y = index.sigmap(anot->getPort("\\Y"));
			pool<ModIndex::PortInfo> yports = index.query_ports(y);
			for(auto x : yports)
			{
				//Skip anything that isn't the same TFF type as us
				if(x.cell == current_cell)
					continue;
				if(x.cell->type != current_cell->type)
					continue;

				//And we must be feeding into its T input
				if(x.port != "\\T")
					continue;

				//Verify that we share the same clock and reset
				if(anchor_reset != index.sigmap(x.cell->getPort("\\R")))
					continue;
				if(anchor_clock != index.sigmap(x.cell->getPort("\\C")))
					continue;

				//Found it
				current_cell = x.cell;
				log("    Found flipflop in chain: %s\n", current_cell->name.c_str());
				hit = true;
			}
		}

		if(!hit)
			break;
	}
	log("  Found %zu downstream T flipflops\n", downstream_flipflops.size());

	//Find the upstream flipflop (LSB)
	//This should be a DFF connected to one of our tdriver's inputs.
	auto tdriver_a = GetDriverOfPort(tdriver.cell, "\\A", "\\Q", index);
	auto tdriver_b = GetDriverOfPort(tdriver.cell, "\\B", "\\Q", index);
	auto type = cell->type.str().substr(6);		//"PP0_" or similar
	log("  type=%s\n", type.c_str());
	if(tdriver_a.cell)
	{
		auto ttype = tdriver_a.cell->type.str().substr(5);
		log("  ttype=%s\n", ttype.c_str());
		if( (tdriver_a.cell->type.str().find("$_DFF") == 0) && (ttype == type) )
			log("  Hit on port A\n");
	}
	if(tdriver_b.cell)
	{
		auto ttype = tdriver_b.cell->type.str().substr(5);
		log("  ttype=%s\n", ttype.c_str());
		if( (tdriver_b.cell->type.str().find("$_DFF") == 0) && (ttype == type) )
			log("  Hit on port B\n");
	}

	/*
	pool<Cell*> rval;
	for(auto b : port)
	{
		pool<ModIndex::PortInfo> ports = index.query_ports(b);
		for(auto x : ports)
		{
			if(x.cell == src)
				continue;
			rval.insert(x.cell);
		}
	}
	return rval;
	*/

	/*
	//A input is the count value. Check if it has COUNT_EXTRACT set.
	//If it's not a wire, don't even try
	auto port = sigmap(cell->getPort("\\A"));
	if(!port.is_wire())
		return;
	RTLIL::Wire* a_wire = port.as_wire();
	bool force_extract = false;
	bool never_extract = false;
	string count_reg_src = a_wire->attributes["\\src"].decode_string().c_str();
	if(a_wire->attributes.find("\\COUNT_EXTRACT") != a_wire->attributes.end())
	{
		pool<string> sa = a_wire->get_strpool_attribute("\\COUNT_EXTRACT");
		string extract_value;
		if(sa.size() >= 1)
		{
			extract_value = *sa.begin();
			log("  Signal %s declared at %s has COUNT_EXTRACT = %s\n",
				log_id(a_wire),
				count_reg_src.c_str(),
				extract_value.c_str());

			if(extract_value == "FORCE")
				force_extract = true;
			else if(extract_value == "NO")
				never_extract = true;
			else if(extract_value == "AUTO")
			{}	//default
			else
				log_error("  Illegal COUNT_EXTRACT value %s (must be one of FORCE, NO, AUTO)\n",
					extract_value.c_str());
		}
	}

	//If we're explicitly told not to extract, don't infer a counter
	if(never_extract)
		return;

	//Attempt to extract a counter
	CounterExtraction extract;
	int reason = greenpak4_counters_tryextract(index, cell, extract);

	//Nonzero code - we could not find a matchable counter.
	//Do nothing, unless extraction was forced in which case give an error
	if(reason != 0)
	{
		static const char* reasons[24]=
		{
			"no problem",									//0
			"counter is larger than 14 bits",				//1
			"counter does not count by one",				//2
			"counter uses signed math",						//3
			"counter does not count by one",				//4
			"ALU is not a subtractor",						//5
			"ALU is not a subtractor",						//6
			"ALU ports used outside counter",				//7
			"ALU ports used outside counter",				//8
			"ALU output used outside counter",				//9
			"ALU output is not a mux",						//10
			"ALU output is not full bus",					//11
			"Underflow value is not constant",				//12
			"No underflow detector found",					//13
			"Mux output is used outside counter",			//14
			"Counter reg is not DFF/ADFF",					//15
			"Counter input is not full bus",				//16
			"Count register is used outside counter, but not by a DCMP or DAC",		//17
			"Register output is not full bus",				//18
			"Register output is not full bus",				//19
			"No init value found",							//20
			"Underflow value is not equal to init value",	//21
			"Reset polarity is not positive",				//22
			"Reset is not to zero"							//23
		};

		if(force_extract)
		{
			log_error(
			"Counter extraction is set to FORCE on register %s, but a counter could not be inferred (%s)\n",
			log_id(a_wire),
			reasons[reason]);
		}
		return;
	}

	//Figure out the final cell type based on the counter size
	string celltype = "\\GP_COUNT8";
	if(extract.width > 8)
		celltype = "\\GP_COUNT14";

	//Get new cell name
	string countname = string("$auto$GP_COUNTx$") + log_id(extract.rwire->name.str());

	//Log it
	total_counters ++;
	string reset_type = "non-resettable";
	if(extract.has_reset)
	{
		//TODO: support other kind of reset
		reset_type = "async resettable";
	}
	log("  Found %d-bit %s down counter %s (counting from %d) for register %s declared at %s\n",
		extract.width,
		reset_type.c_str(),
		countname.c_str(),
		extract.count_value,
		log_id(extract.rwire->name),
		count_reg_src.c_str());

	//Wipe all of the old connections to the ALU
	cell->unsetPort("\\A");
	cell->unsetPort("\\B");
	cell->unsetPort("\\BI");
	cell->unsetPort("\\CI");
	cell->unsetPort("\\CO");
	cell->unsetPort("\\X");
	cell->unsetPort("\\Y");
	cell->unsetParam("\\A_SIGNED");
	cell->unsetParam("\\A_WIDTH");
	cell->unsetParam("\\B_SIGNED");
	cell->unsetParam("\\B_WIDTH");
	cell->unsetParam("\\Y_WIDTH");

	//Change the cell type
	cell->type = celltype;

	//Hook up resets
	if(extract.has_reset)
	{
		//TODO: support other kinds of reset
		cell->setParam("\\RESET_MODE", RTLIL::Const("LEVEL"));
		cell->setPort("\\RST", extract.rst);
	}
	else
	{
		cell->setParam("\\RESET_MODE", RTLIL::Const("RISING"));
		cell->setPort("\\RST", RTLIL::SigSpec(false));
	}

	//Hook up other stuff
	cell->setParam("\\CLKIN_DIVIDE", RTLIL::Const(1));
	cell->setParam("\\COUNT_TO", RTLIL::Const(extract.count_value));

	cell->setPort("\\CLK", extract.clk);
	cell->setPort("\\OUT", extract.outsig);

	//Hook up any parallel outputs
	for(auto load : extract.pouts)
	{
		log("    Counter has parallel output to cell %s port %s\n", log_id(load.cell->name), log_id(load.port));

		//Find the wire hooked to the old port
		auto sig = load.cell->getPort(load.port);

		//Connect it to our parallel output
		//(this is OK to do more than once b/c they all go to the same place)
		cell->setPort("\\POUT", sig);
	}

	//Delete the cells we've replaced (let opt_clean handle deleting the now-redundant wires)
	cells_to_remove.insert(extract.count_mux);
	cells_to_remove.insert(extract.count_reg);
	cells_to_remove.insert(extract.underflow_inv);

	//Finally, rename the cell
	cells_to_rename.insert(pair<Cell*, string>(cell, countname));
	*/
}

struct RecoverTFFCountersPass : public Pass {
	RecoverTFFCountersPass() : Pass("recover_tff_counters", "Extract TFF based counters") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    recover_tff_counters [selection]\n");
		log("\n");
		log("This pass converts chains of T flipflops into counters\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header(design, "Executing RECOVER_TFF_COUNTERS pass (finding T flipflop based counters)\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			// if (args[argidx] == "-v") {
			// 	continue;
			// }
			break;
		}
		extra_args(args, argidx, design);

		//Extract all of the counters we could find
		unsigned int total_counters = 0;
		for (auto module : design->selected_modules())
		{
			pool<Cell*> cells_to_remove;

			ModIndex index(module);
			for (auto cell : module->selected_cells())
				recover_tff_counters_worker(index, cell, total_counters, cells_to_remove);

			for(auto cell : cells_to_remove)
			{
				log("Removing cell %s (%s)\n", log_id(cell->name), cell->type.c_str());
				module->remove(cell);
			}
		}

		if(total_counters)
			log("Extracted %u counters\n", total_counters);
	}
} RecoverTFFCountersPass;

PRIVATE_NAMESPACE_END
