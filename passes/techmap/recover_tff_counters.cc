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
	unsigned int& total_counters,
	pool<Cell*>& cells_to_remove)
{
	SigMap& sigmap = index.sigmap;
	auto module = cell->module;

	//Start by looking at TFFs
	if (cell->type.str().find("_DFF") == string::npos)
		return;

	//Find the driver of our D input. It should be an inverter for the LSB, which is our anchor cell.
	auto tdriver = GetDriverOfPort(cell, "\\D", "\\Y", index);
	if(tdriver == ModIndex::PortInfo())
		return;
	if(tdriver.cell->type != "$_NOT_")
		return;

	//Verify that the inverter is driven by our output
	auto anchordriver = GetDriverOfPort(tdriver.cell, "\\A", "\\Q", index);
	if(anchordriver == ModIndex::PortInfo())
		return;
	if(anchordriver.cell != cell)
		return;

	//TODO: Figure out how to recover reset value etc

	//log("Found candidate counter LSB at %s (%s)\n", log_id(cell->name), cell->type.c_str());

	//Look down the counter toward the MSB.
	//We should see a chain of AND-NOTs and TFFs, sharing our reset and clock inputs
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
			//Skip back edges
			if(x.cell == current_cell)
				continue;

			//For the first element of the chain, there's just a NOT since the -1th element of the counter doesn't exist
			if(downstream_flipflops.empty())
			{
				//Skip anything that isn't an inverter
				if(x.cell->type != "$_NOT_")
					continue;

				//The inverter's port must be A (input)
				if(x.port != "\\A")
					continue;
			}

			//Otherwise, we expect a chain of AND-NOT cells
			else
			{
				//Skip anything that isn't a ANDNOT
				if(x.cell->type != "$_ANDNOT_")
					continue;

				//The ANDNOT's port must be B (the inverting output)
				if(x.port != "\\B")
					continue;

				//The A input of the same ANDNOT cell must be our toggle input (the previous chain element's output)
				auto noninv = GetDriverOfPort(x.cell, "\\A", "\\Y", index);
				if(noninv == ModIndex::PortInfo())
					continue;
				auto expected_driver = GetDriverOfPort(current_cell, "\\T", "\\Y", index);
				if(noninv.cell != expected_driver.cell)
					continue;
			}

			//Match if we got this far
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
				//Make sure we didn't find a back edge!
				if(x.cell == current_cell)
					continue;

				//We must be feeding into its T input
				if(x.port != "\\T")
					continue;

				//Verify that we share the same clock and reset
				if(anchor_reset != index.sigmap(x.cell->getPort("\\R")))
					continue;
				if(anchor_clock != index.sigmap(x.cell->getPort("\\C")))
					continue;

				//Found it
				current_cell = x.cell;
				//log("    Found flipflop in chain: %s (%s)\n", current_cell->name.c_str(), current_cell->type.c_str());
				hit = true;
			}
		}

		if(!hit)
			break;
	}
	//log("  Found %zu downstream T flipflops\n", downstream_flipflops.size());

	//If less than 3 bits long, don't extract
	//TODO: make this configurable
	int count_width = 1 + downstream_flipflops.size();
	if(count_width < 3)
		return;

	log("  Converting T flipflops %s ... %s to a %d-bit down counter\n",
		log_id(cell->name),
		log_id(downstream_flipflops[downstream_flipflops.size()-1]->name),
		count_width
	);

	//TODO: do this!
	log_warning("  Not copying INIT attributes from incoming TFFs\n");
	log_warning("  Not checking set/reset polarity on original TFFs\n");

	//Update stats - we have a hit!
	total_counters ++;

	//Create the new $_COUNT cell
	auto counter = module->addCell(NEW_ID, "$__COUNT_");
	counter->setParam("\\RESET_MODE", RTLIL::Const("FIXME"));	//not yet implemented
	counter->setParam("\\WIDTH", RTLIL::Const(count_width));
	counter->setParam("\\COUNT_TO", RTLIL::Const(pow(2, count_width) - 1);
	counter->setParam("\\HAS_CE", RTLIL::Const(0));
	counter->setParam("\\HAS_POUT", RTLIL::Const(1));
	counter->setParam("\\DIRECTION", RTLIL::Const("DOWN"));
	counter->setPort("\\CE", RTLIL::Const(1));
	counter->setPort("\\UP", RTLIL::Const(0));
	counter->setPort("\\RST", cell->getPort("\\R"));
	counter->setPort("\\CLK", cell->getPort("\\C"));
	//no OUT connection yet

	//Drive POUT to the chain of TFFs
	RTLIL::SigSpec outbus;
	outbus.append(cell->getPort("\\Q"));
	for(auto ff : downstream_flipflops)
		outbus.append(ff->getPort("\\Q"));
	counter->setPort("\\POUT", outbus);

	//Delete the cells we've replaced (let opt_clean handle deleting the now-redundant wires)
	cells_to_remove.insert(cell);
	for(auto ff : downstream_flipflops)
		cells_to_remove.insert(ff);
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
		log("You should extract T flipflops by running \"recover_tff\" followed by\n");
		log("mapping the design with \"abc -g ANDNOT\" before running this pass\n");
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
			log("Extracted %u counter(s)\n", total_counters);
	}
} RecoverTFFCountersPass;

PRIVATE_NAMESPACE_END
