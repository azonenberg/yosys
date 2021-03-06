/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2017 Robert Ou <rqou@robertou.com>
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

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct RecoverAdderCorePass : public Pass {
    RecoverAdderCorePass() : Pass("recover_adder_core", "converts adder chains into $alu/$add/$sub") { }
    virtual void help()
    {
        //   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
        log("\n");
        log("    recover_adder_core\n");
        log("\n");
        log("converts adder chains into $alu/$add/$sub\n");
        log("\n");
        log("This performs the core step of the recover_adder command. This step recognizes\n");
        log("chains of adders found by the previous steps and converts these chains into one\n");
        log("logical cell.\n");
        log("\n");
    }
    virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
    {
        (void)args;

        for (auto module : design->selected_modules())
        {
            SigMap sigmap(module);

            // Find all the adder/subtractor cells from the previous extract step
            pool<Cell*> addsub_cells;
            for (auto cell : module->selected_cells())
            {
                if (cell->type == "\\__HALF_ADDER_" || cell->type == "\\__FULL_ADDER_" ||
                    cell->type == "\\__HALF_SUBTRACTOR_" || cell->type == "\\__FULL_SUBTRACTOR_")
                {
                    log("Found adder/subtractor cell %s\n", cell->name.c_str());
                    addsub_cells.insert(cell);
                }
            }

            // Find all the carry-related wires
            dict<SigBit, pool<Cell*>> carry_wires;
            for (auto cell : addsub_cells)
            {
                if (cell->type == "\\__HALF_ADDER_")
                {
                    auto w = sigmap(cell->getPort("\\Cout"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();
                }
                else if (cell->type == "\\__HALF_SUBTRACTOR_")
                {
                    auto w = sigmap(cell->getPort("\\Bout"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();
                }
                else if (cell->type == "\\__FULL_ADDER_")
                {
                    auto w = sigmap(cell->getPort("\\Cout"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();

                    w = sigmap(cell->getPort("\\Cin"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();
                }
                else if (cell->type == "\\__FULL_SUBTRACTOR_")
                {
                    auto w = sigmap(cell->getPort("\\Bout"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();

                    w = sigmap(cell->getPort("\\Bin"));
                    log_assert(w.size() == 1);
                    carry_wires[w[0]] = pool<Cell*>();
                }
                else
                {
                    log_assert(0);
                }
            }

            // Find all the other cells that touch each carry wire
            for (auto cell : module->selected_cells())
            {
                for (auto &conn: cell->connections())
                {
                    for (auto sig : sigmap(conn.second))
                    {
                        // FIXME: Do we care about checking multiple drivers?
                        if (carry_wires.count(sig))
                        {
                            log("Carry wire goes into cell %s\n", cell->name.c_str());
                            carry_wires[sig].insert(cell);
                        }
                    }
                }
            }

            // Need to find fan-outs into module ports
            pool<SigBit> carry_fanout_to_port;
            for (auto wire : module->selected_wires())
            {
                if (wire->port_output)
                {
                    for (auto bit : sigmap(wire))
                    {
                        if (carry_wires.count(bit))
                        {
                            log("Found a carry fanout to port %s\n", wire->name.c_str());
                            carry_fanout_to_port.insert(bit);
                        }
                    }
                }
            }

            // Do the actual adder extraction logic
            pool<Cell*> consumed_cells;
            for (auto cell : addsub_cells)
            {
                if (consumed_cells.count(cell))
                    continue;

                log("Working on cell %s...\n", cell->name.c_str());

                bool is_sub = (cell->type == "\\__HALF_SUBTRACTOR_" || cell->type == "\\__FULL_SUBTRACTOR_");
                bool has_carryin = false;
                bool has_carryout = false;
                bool has_carry_fanout = false;
                std::deque<Cell*> cur_adder = {cell};

                // Extend "left"
                Cell* x = cell;
                while (true)
                {
                    // This has to be the first bit
                    if (x->type == "\\__HALF_ADDER_" || x->type == "\\__HALF_SUBTRACTOR_")
                        break;

                    auto c = sigmap(x->getPort(is_sub ? "\\Bin" : "\\Cin"))[0];
                    // What else is connected to this wire?
                    pool<Cell*> other_carry_cells = pool<Cell*>(carry_wires[c]);
                    other_carry_cells.erase(x);

                    size_t connected_addsubs = 0;
                    Cell* connected_addsub = 0;
                    for (auto y : other_carry_cells)
                    {
                        if (consumed_cells.count(y))
                            continue;

                        if (is_sub)
                        {
                            if (y->type == "\\__HALF_SUBTRACTOR_" || y->type == "\\__FULL_SUBTRACTOR_")
                            {
                                // Needs to be driven by the Bout wire
                                if (sigmap(y->getPort("\\Bout"))[0] == c)
                                {
                                    connected_addsub = y;
                                    connected_addsubs++;
                                }
                            }
                        }
                        else
                        {
                            if (y->type == "\\__HALF_ADDER_" || y->type == "\\__FULL_ADDER_")
                            {
                                // Needs to be driven by the Cout wire
                                if (sigmap(y->getPort("\\Cout"))[0] == c)
                                {
                                    connected_addsub = y;
                                    connected_addsubs++;
                                }
                            }
                        }
                    }
                    log("  This cell is connected to %zu/%zu cells\n", connected_addsubs, other_carry_cells.size());

                    if (connected_addsubs == 0)
                    {
                        // This has to be the end of the chain
                        has_carryin = true;
                        break;
                    }
                    else
                    {
                        if (connected_addsubs > 1)
                        {
                            // Break the chain here
                            // FIXME: Do we want to find the "longest" adder? Do we do that in a later pass instead?
                            has_carryin = true;
                            break;
                        }
                        else
                        {
                            // The cell we are examining is connected to one and only one other adder/subtractor cell
                            if (other_carry_cells.size() > 1 || carry_fanout_to_port.count(c))
                                has_carry_fanout = true;

                            log("  Absorbing cell %s (left)\n", connected_addsub->name.c_str());
                            cur_adder.push_front(connected_addsub);
                            x = connected_addsub;
                        }
                    }
                }

                // Extend "right"
                // FIXME: Deduplicate code?
                x = cell;
                while (true)
                {
                    // This has to be the last bit
                    if (x->type == "\\__XOR3_")
                        break;

                    auto c = sigmap(x->getPort(is_sub ? "\\Bout" : "\\Cout"))[0];
                    // What else is connected to this wire?
                    pool<Cell*> other_carry_cells = pool<Cell*>(carry_wires[c]);
                    other_carry_cells.erase(x);

                    size_t connected_addsubs = 0;
                    Cell* connected_addsub = 0;
                    for (auto y : other_carry_cells)
                    {
                        if (consumed_cells.count(y))
                            continue;

                        if (is_sub)
                        {
                            if (y->type == "\\__XOR3_" || y->type == "\\__FULL_SUBTRACTOR_")
                            {
                                // XXX we assume we're driving this wire and nobody else is. Is this safe?
                                connected_addsub = y;
                                connected_addsubs++;
                            }
                        }
                        else
                        {
                            if (y->type == "\\__XOR3_" || y->type == "\\__FULL_ADDER_")
                            {
                                // XXX we assume we're driving this wire and nobody else is. Is this safe?
                                connected_addsub = y;
                                connected_addsubs++;
                            }
                        }
                    }
                    log("  This cell is connected to %zu/%zu cells\n", connected_addsubs, other_carry_cells.size());

                    if (connected_addsubs == 0)
                    {
                        // This has to be the end of the chain
                        has_carryout = true;
                        break;
                    }
                    else
                    {
                        if (connected_addsubs > 1)
                        {
                            // Break the chain here
                            // FIXME: Do we want to find the "longest" adder? Do we do that in a later pass instead?
                            has_carryout = true;
                            break;
                        }
                        else
                        {
                            // The cell we are examining is connected to one and only one other adder/subtractor cell
                            if (other_carry_cells.size() > 1 || carry_fanout_to_port.count(c))
                                has_carry_fanout = true;

                            log("  Absorbing cell %s (right)\n", connected_addsub->name.c_str());
                            cur_adder.push_back(connected_addsub);
                            x = connected_addsub;
                        }
                    }
                }

                if (cur_adder.size() > 1)
                {
                    // We found an actual adder
                    log("An adder/subtractor was found!\n");
                    log("  Add/sub: %s\n", is_sub ? "sub" : "add");
                    log("  Carry-in: %s\n", has_carryin ? "yes" : "no");
                    log("  Carry-out: %s\n", has_carryout ? "yes" : "no");
                    log("  Carry fanouts: %s\n", has_carry_fanout ? "yes" : "no");
                    for (auto x : cur_adder)
                        log("    %s\n", x->name.c_str());

                    if (!has_carry_fanout)
                    {
                        // Can generate an $add/$sub cell
                        SigSpec a;
                        SigSpec b;
                        SigSpec y;

                        for (size_t i = 0; i < cur_adder.size(); i++)
                        {
                            auto x = cur_adder[i];
                            auto this_a = x->getPort("\\A")[0];
                            auto this_b = x->getPort("\\B")[0];
                            if (x->type == "\\__XOR3_")
                            {
                                auto this_c = x->getPort("\\C")[0];
                                SigBit last_cout;
                                if (!is_sub)
                                    last_cout = cur_adder[i - 1]->getPort("\\Cout")[0];
                                else
                                    last_cout = cur_adder[i - 1]->getPort("\\Bout")[0];

                                if (sigmap(this_a) == sigmap(last_cout))
                                    this_a = this_c;
                                else if (sigmap(this_b) == sigmap(last_cout))
                                    this_b = this_c;
                                else
                                    log_assert(sigmap(this_c) == sigmap(last_cout));
                            }
                            a.append_bit(this_a);
                            b.append_bit(this_b);
                            y.append_bit(x->getPort("\\Y")[0]);
                        }

                        if (has_carryout)
                        {
                            if (!is_sub)
                                y.append_bit(cur_adder.back()->getPort("\\Cout")[0]);
                            else
                                y.append_bit(cur_adder.back()->getPort("\\Bout")[0]);
                        }

                        auto addsub_new_cell = module->addCell(NEW_ID, is_sub ? "$sub" : "$add");
                        addsub_new_cell->setParam("\\A_SIGNED", 0);
                        addsub_new_cell->setParam("\\B_SIGNED", 0);
                        addsub_new_cell->setParam("\\A_WIDTH", a.size());
                        addsub_new_cell->setParam("\\B_WIDTH", b.size());
                        addsub_new_cell->setParam("\\Y_WIDTH", y.size());
                        addsub_new_cell->setPort("\\A", a);
                        addsub_new_cell->setPort("\\B", b);
                        addsub_new_cell->setPort("\\Y", y);

                        if (has_carryin)
                        {
                            auto intermed_wires = module->addWire(NEW_ID, y.size());
                            addsub_new_cell->setPort("\\Y", intermed_wires);

                            auto carryin_new_cell = module->addCell(NEW_ID, is_sub ? "$sub" : "$add");
                            carryin_new_cell->setParam("\\A_SIGNED", 0);
                            carryin_new_cell->setParam("\\B_SIGNED", 0);
                            carryin_new_cell->setParam("\\A_WIDTH", y.size());
                            carryin_new_cell->setParam("\\B_WIDTH", 1);
                            carryin_new_cell->setParam("\\Y_WIDTH", y.size());
                            carryin_new_cell->setPort("\\A", intermed_wires);
                            if (!is_sub)
                                carryin_new_cell->setPort("\\B", cur_adder.front()->getPort("\\Cin")[0]);
                            else
                                carryin_new_cell->setPort("\\B", cur_adder.front()->getPort("\\Bin")[0]);
                            carryin_new_cell->setPort("\\Y", y);
                        }
                    }
                    else
                    {
                        // Generate an $alu cell
                        SigSpec a;
                        SigSpec b;
                        SigSpec y;
                        SigSpec cout;

                        for (size_t i = 0; i < cur_adder.size(); i++)
                        {
                            auto x = cur_adder[i];
                            auto this_a = x->getPort("\\A")[0];
                            auto this_b = x->getPort("\\B")[0];
                            if (x->type == "\\__XOR3_")
                            {
                                auto this_c = x->getPort("\\C")[0];
                                SigBit last_cout;
                                if (!is_sub)
                                    last_cout = cur_adder[i - 1]->getPort("\\Cout")[0];
                                else
                                    last_cout = cur_adder[i - 1]->getPort("\\Bout")[0];

                                if (sigmap(this_a) == sigmap(last_cout))
                                    this_a = this_c;
                                else if (sigmap(this_b) == sigmap(last_cout))
                                    this_b = this_c;
                                else
                                    log_assert(sigmap(this_c) == sigmap(last_cout));
                            }
                            a.append_bit(this_a);
                            b.append_bit(this_b);
                            y.append_bit(x->getPort("\\Y")[0]);
                            auto portname = is_sub ? "\\Bout" : "\\Cout";
                            if (x->hasPort(portname))
                                cout.append_bit(x->getPort(portname)[0]);
                            else
                                cout.append_bit(module->addWire(NEW_ID));
                        }

                        auto alu_new_cell = module->addCell(NEW_ID, "$alu");
                        alu_new_cell->setParam("\\A_SIGNED", 0);
                        alu_new_cell->setParam("\\B_SIGNED", 0);
                        alu_new_cell->setParam("\\A_WIDTH", a.size());
                        alu_new_cell->setParam("\\B_WIDTH", b.size());
                        alu_new_cell->setParam("\\Y_WIDTH", y.size());
                        alu_new_cell->setPort("\\A", a);
                        alu_new_cell->setPort("\\B", b);
                        alu_new_cell->setPort("\\X", module->addWire(NEW_ID, y.size()));
                        alu_new_cell->setPort("\\Y", y);
                        if (!is_sub)
                        {
                            alu_new_cell->setPort("\\BI", false);
                            alu_new_cell->setPort("\\CO", cout);
                            if (has_carryin)
                                alu_new_cell->setPort("\\CI", cur_adder.front()->getPort("\\Cin")[0]);
                            else
                                alu_new_cell->setPort("\\CI", false);
                        }
                        else
                        {
                            alu_new_cell->setPort("\\BI", true);

                            auto carryout_invert_wires = module->addWire(NEW_ID, cout.size());
                            auto carryout_invert_cell = module->addCell(NEW_ID, "$not");
                            carryout_invert_cell->setParam("\\A_SIGNED", 0);
                            carryout_invert_cell->setParam("\\A_WIDTH", cout.size());
                            carryout_invert_cell->setParam("\\Y_WIDTH", cout.size());
                            carryout_invert_cell->setPort("\\A", carryout_invert_wires);
                            carryout_invert_cell->setPort("\\Y", cout);
                            alu_new_cell->setPort("\\CO", carryout_invert_wires);

                            if (has_carryin)
                            {
                                auto carryin_invert_wire = module->addWire(NEW_ID);
                                auto carryin_invert_cell = module->addCell(NEW_ID, "$not");
                                carryin_invert_cell->setParam("\\A_SIGNED", 0);
                                carryin_invert_cell->setParam("\\A_WIDTH", 1);
                                carryin_invert_cell->setParam("\\Y_WIDTH", 1);
                                carryin_invert_cell->setPort("\\A", cur_adder.front()->getPort("\\Bin")[0]);
                                carryin_invert_cell->setPort("\\Y", carryin_invert_wire);
                                alu_new_cell->setPort("\\CI", carryin_invert_wire);
                            }
                            else
                                alu_new_cell->setPort("\\CI", true);
                        }
                    }

                    // Mark all of these cells as removed
                    for (auto x : cur_adder)
                        consumed_cells.insert(x);
                }
            }

            // Remove every cell that we've used up
            for (auto cell : consumed_cells)
                module->remove(cell);
        }
    }
} RecoverAdderCorePass;

PRIVATE_NAMESPACE_END
