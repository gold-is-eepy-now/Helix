# LOOM prior art and research notes

Status of this document: written from memory in June 2026's knowledge, with a few checks by web
search where noted. Citations below were not all re-read in full. Treat each entry as a pointer to
verify, not as a settled reading. Where a name could not be confirmed, it is marked.

## Method

For each system: what it achieved, its similarity to LOOM, its difference, which LOOM features it
already establishes, and what remains. The brief warns against claiming novelty for any single
feature, so the test here is the combination the brief names: first-class Continuities, morphable
topologies, end-to-end resource ownership, semantic correctness enforcement, and dynamic
substitution under declared contracts.

## Entries

### Exokernel (Engler, Kaashoek, O'Toole, MIT, 1995)

- **Achieved:** the kernel exports hardware resources through secure bindings and leaves abstractions
  to libraries (library operating systems). Protection is separated from management.
- **Similar to LOOM:** the kernel enforces, and policy lives outside it.
- **Different:** Exokernel's unit is a hardware resource, not a behaviour with a contract. It has no
  notion of alternative implementations.
- **Already established:** protection outside the policy layer. The Weaver/kernel split follows this.

### Barrelfish (Baumann et al., ETH Zürich and Microsoft Research, SOSP 2009)

- **Achieved:** a multikernel. One kernel per core, communicating by message passing; the system is
  modelled as a distributed system; placement and resource decisions use a system knowledge base.
- **Similar to LOOM:** heterogeneous hardware, explicit messages, and a runtime that reasons about
  placement.
- **Different:** no contracts and no morphologies. Barrelfish does not substitute implementations under
  a correctness contract.
- **Already established:** message passing across heterogeneous cores; a knowledge base for placement.

### FOS (Wentzlaff et al., Princeton, 2009–2010, "factored operating system")

- **Achieved:** splits the OS into services that run on separate cores and scale by replication.
- **Similar to LOOM:** an OS made of services, not one monolithic kernel.
- **Different:** the service decomposition is about scalability; there is no per-behaviour contract.

### Eclipse (Bruno et al., USENIX ATC 1998, "The Eclipse Operating System: Providing Quality of Service…")

- **Achieved:** an operating system that provides quality of service, so an application can get
  reserved resources. Checked by web search: the USENIX paper exists under that title. Its full
  mechanism was not read in this session.
- **Similar to LOOM:** resource reservation with admission; the application's view of its resources.
- **Different:** Eclipse reserves CPU and I/O for processes. It does not switch implementations, and
  there is no correctness class beyond "meets the reservation".
- **Already established:** admission control and reservations. LOOM's Field follows this line.
- **Relevance:** the brief asks for QoS reservation systems. Eclipse is the closest one checked.

### Active device drivers (Ryzhyk, Zhang, Heiser et al., NICTA/UNSW, c. 2010–2012)

- **Achieved:** a model of device drivers in which the driver is an active component that
  interacts with the device and its environment through messages and verified interfaces. Checked by
  web search: "The case for active device drivers" (APSys 2010) and "Active device drivers" (NICTA tech
  report 2012) exist.
- **Similar to LOOM:** drivers as isolated, message-based components. Device behaviour as a
  contract.
- **Different:** no morphologies and no dynamic substitution under a contract. The focus is driver
  correctness and isolation, not resource-driven choice.

### CEDR (Mack et al., arXiv 2204.08962, "CEDR: a compiler-integrated, extensible DSSoC runtime")

- **Achieved:** a runtime that schedules tasks across heterogeneous accelerators on
  domain-specific SoCs, with the application compiled against the runtime's API. Checked by web
  search: the paper exists.
- **Similar to LOOM:** dynamic choice of a processing element per task, and a model of accelerators
  as alternatives.
- **Different:** CEDR is a user-space runtime, not a kernel. Its alternatives are per-task kernel
  choices, not a continuous behaviour with state migration and a contract.
- **Already established:** dynamic task-to-accelerator mapping, which is part of LOOM's Weaver job.

### Taskflow (Huang et al., heterogeneous task graphs, C++)

- **Achieved:** a C++ library for task-parallel and heterogeneous task graphs, with dynamic tasks.
- **Similar to LOOM:** dynamic task graphs; GPU and CPU tasks in one graph.
- **Different:** a library, not a kernel. Acyclic task graphs with dynamic extensions. LOOM's brief
  asks for cyclic structures, which Taskflow's model does not target.

### TERSE (not confirmed)

The brief lists TERSE. A web search for an operating-system project of that name did not return one.
Until its full name and venue are known, nothing is claimed about it. Please supply the reference.

### Stramash and heterogeneous-ISA kernels (Virginia Tech SSRG)

- **Achieved:** operating systems that fuse kernels across heterogeneous ISAs on cache-coherent
  platforms, seen in a web search.
- **Similar to LOOM:** heterogeneous execution inside one OS.
- **Different:** the unit is still processes and threads; no contract-driven substitution.

### Component-based systems (seL4 with CAmkES, Genode, Fuchsia components)

- **Achieved:** isolated components with typed interfaces, composed by a manifest.
- **Similar to LOOM:** typed interfaces and isolation of components (LOOM's Cells).
- **Different:** components are fixed at composition time. A Morphology switch at run time is not
  part of these systems' models. Named from memory; not verified in this session.

### QoS resource containers (Banga, Druschel, Mogul, 1999, "Resource containers")

- **Achieved:** resource accounting that follows a computation rather than a process.
- **Similar to LOOM:** resource ownership attached to the unit of work. This matches the brief's
  "end-to-end resource ownership". Named from memory; not verified in this session.
- **Different:** no contracts or morphologies.

### Dataflow and feedback systems (StreamIt, Naiad, Timely Dataflow, Dryad)

- **Achieved:** dataflow computation with, in some cases, cyclic iteration (Naiad and Timely
  Dataflow support feedback loops with progress tracking).
- **Similar to LOOM:** feedback loops (brief section 5). Timely Dataflow's progress tracking is a
  worked answer to the "event ordering" question LOOM still has open.
- **Different:** these are user-space dataflow engines, not kernels, and they do not substitute
  implementations under a correctness contract. Named from memory; not verified in this session.

## What is already established

- Kernel enforcement separated from policy (Exokernel).
- Reservations and admission control for QoS (Eclipse, resource containers).
- Message-passing, component-isolated drivers (active device drivers, Barrelfish, component systems).
- Dynamic choice of processing element for tasks (CEDR, Taskflow).
- Dataflow with feedback loops and progress tracking (Naiad, Timely Dataflow).

Each item above was built and studied by others. LOOM reuses them. None is claimed as new.

## What may be new (to be tested, not assumed)

The brief's proposed contribution is the combination:

1. **Continuities as first-class kernel objects.** Checked: the closest work treats the unit as a
   task, a process or a component. Continuity is the same idea as "a service with a contract", but
   nothing checked makes the contract a kernel-enforced object that the kernel rejects on violation.
2. **Morphable topologies with state migration.** Checked: CEDR switches processing elements per
   task; none checked migrates a continuous behaviour's state between arrangements under a contract.
   Unconfirmed; a literature search with the right terms would be the next step.
3. **End-to-end resource ownership.** Partly established (resource containers). The open part is
   the extension to a multi-dimensional field where kinds are not interchangeable.
4. **Semantic correctness enforcement.** Established in part: type and quality contracts exist in
   language and runtime systems. The kernel-enforced version is less common, but not checked
   exhaustively.
5. **Dynamic substitution under declared contracts.** Not found in the checked systems. Unconfirmed.

## Open research questions

- **Feedback loops.** How does the kernel guarantee progress when a Cell is in a cycle? Timely
  Dataflow's progress tracking is a candidate, but it targets a single engine.
- **Backpressure and buffer semantics.** Not designed yet.
- **Deadlock detection.** Not designed. A cycle of Cells waiting on each other needs a wait-for graph
  or timeouts with a defined fallback Morphology.
- **Migration cost.** The simulator assumes it is free. A real measurement is needed before the design
  claims it is practical.
- **Contract checking in general.** The kernel checks quality numbers. It cannot check that a
  decoder's output is correct. That needs a test or proof owned by the Continuity.
- **Weaver as a privileged service.** How does a userspace Weaver get the Field's view safely and
  quickly enough?

## Sources

- Engler, Kaashoek, O'Toole, "Exokernel: An Operating System Architecture for Application-Level Resource Management", SOSP 1995.
- Baumann et al., "The Multikernel: A New OS Architecture for Scalable Multicore Systems", SOSP 2009.
- Bruno et al., "The Eclipse Operating System: Providing Quality of Service…" (title truncated as in the search result), USENIX ATC 1998. https://www.usenix.org/legacy/publications/library/proceedings/usenix98/full_papers/bruno/bruno_html/bruno.html
- Ryzhyk et al., "The Case for Active Device Drivers", APSys 2010. https://www.doc.ic.ac.uk/~afd/papers/2011/APSys.pdf
- Amani et al., "Active Device Drivers" (NICTA technical report). https://www.trustworthy.systems/publications/nictaabstracts/Amani_CDLRZ_12:tr.abstract
- Mack et al., "CEDR: A Compiler-integrated, Extensible DSSoC Runtime", arXiv:2204.08962. https://arxiv.org/pdf/2204.08962
- Stramash (Virginia Tech SSRG). https://vtechworks.lib.vt.edu/items/53fd7dfe-a29a-463d-8b87-31e235000b33
- Banga, Druschel, Mogul, "Resource Containers", OSDI 1999 (from memory).
- Murray et al., "Naiad: A Timely Dataflow System", SOSP 2013 (from memory).
- Taskflow: https://taskflow.github.io (from memory; not re-read).
