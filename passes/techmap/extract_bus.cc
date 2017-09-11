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

void bus_worker(
	ModIndex& index,
	Cell *cell,
	unsigned int& total_buses)
{
	SigMap& sigmap = index.sigmap;

	/*
		Table of ports on each cell that are guaranteed to be a bus.

		Note that with commutative operations such as addition, we cannot make the inputs buses
		because we don't know which bits belong to which input.
		We also cannot infer a bus from
	 */
	//TODO: better way to store this table
	std::set<RTLIL::IdString> busports;
	if(cell->type == "$add")
		busports.emplace("\\Y");
	else if(cell->type == "$__COUNT_")
		busports.emplace("\\POUT");

	//Unknown cell, bus ports not supported
	else
		return;

	//Bus-ify each port
	log("Inferring buses for port(s) of %s\n", log_id(cell->name));
	for(auto p : busports)
	{
		//See what's on the port now
		//If it's a wire, stop - no action needed
		//If there's nothing on the port, skip it as well!
		if(!cell->hasPort(p))
		{
			//log("  Cell does not have a port named %s\n", p.c_str());
			continue;
		}
		auto port = sigmap(cell->getPort(p));
		if(port.is_wire())
		{
			//log("  Port %s is already a wire\n", p.c_str());
			continue;
		}

		//Not a wire, create one!
		auto wire = cell->module->addWire(NEW_ID, port.size());

		//Find downstream entities connected to the old wire and reconnect them
		log("  Inferring bus for port %s\n", p.c_str());
		for(int i=0; i<port.size(); i++)
		{
			auto b = port[i];
			pool<ModIndex::PortInfo> ports = index.query_ports(b);

			log("    Finding additional loads for bit %d / signal %s (found %zu)\n",
				i, log_id(b.wire->name), ports.size());
			for(auto x : ports)
			{
				if(x.cell == cell)
					continue;
				log("      Cell %s port %s is connected to us at offset %d\n",
					log_id(x.cell->name), log_id(x.port), x.offset);

				//Patch the sigspec one bit at a time as needed
				auto dspec = x.cell->getPort(x.port);
				dspec[x.offset] = RTLIL::SigBit(wire, i);
				x.cell->setPort(x.port, dspec);
			}

			//See if this signal is a top-level module port
			//If so, add buffers to preserve the existing signal names
			//TODO: how to handle top level vector ports?
			bool found = false;
			for(auto p : cell->module->ports)
			{
				if(p == b.wire->name)
					found = true;
			}

			if(found)
			{
				log("      Cell %s port %s drives top level module port %s\n",
					log_id(cell->name), p.c_str(), log_id(b.wire->name));

				cell->module->addBufGate(NEW_ID, RTLIL::SigSpec(wire, i, 1), b.wire);
			}
		}

		//Hook up the new wire
		cell->setPort(p, wire);

		//We made a match, note it
		total_buses ++;
	}
}

struct ExtractBusPass : public Pass {
	ExtractBusPass() : Pass("extract_bus", "Extract buses from split nets") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    extract_bus [selection]\n");
		log("\n");
		log("This pass finds buses in a design consisting of single-bit nets\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header(design, "Executing EXTRACT_BUS pass (find buses in netlist).\n");

		//int argidx = 0;
		//extra_args(args, argidx, design);

		//Extract all of the buses we could find
		unsigned int total_buses = 0;
		for (auto module : design->selected_modules())
		{
			ModIndex index(module);
			for (auto cell : module->selected_cells())
				bus_worker(index, cell, total_buses);
		}

		//TODO: Recurse and find additional buses by elimination

		if(total_buses)
			log("Extracted %u buses\n", total_buses);
	}
} ExtractBusPass;

PRIVATE_NAMESPACE_END
