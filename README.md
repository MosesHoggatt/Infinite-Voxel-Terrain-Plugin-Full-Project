Developer's note: 
Thank you for checking out this project! If you want to see some code I've written, start with the [ChunkThreadChild](https://github.com/Endless-98/Infinite-Voxel-Terrain-Plugin-Viewing-Only/blob/main/Source/InfiniteVoxelTerrainPlugin/Private/ChunkThreadChild.cpp) class. My favorite function is GenerateChunkMeshData(), on line 240.

The code in this project is my own work, with minimal use of AI for low-level problem solving, which I then rewrote/refined myself.
This project was developed before I learned git, which is why there is no commit history. I now use git for all of my projects.

I am aware that this project does not perfectly adhere to production standards, as it uses some magic numbers, and there are several monolithic classes that could be broken down and better organized. 
I'd love to rewrite it one day to take into account everything I've learned about proper production-grade code, but despite that, it is a powerful and neat plugin, and I really enjoyed developing it!

[YouTube Trailer](https://www.youtube.com/watch?v=obLBIMXbz2s)

# **Infinite Voxel Terrain Plugin** **Documentation**

Version 0.98  
**This documentation is a work in progress, and is not complete. If you have any questions or comments, please contact us on our [Discord](https://discord.gg/VSCfnzhhVG)\! We will do our best to expand the documentation in the future.**

# **Core Classes**

* [ChunkManager](#~chunkmanager~) \- Sets up ChunkThreads and handles game thread modification of Chunks. Also handles player tracking  
* [ChunkThread](#~chunkthread~) \- Handles the generation of Chunks, including noise generation, voxel generation and mesh generation. ChunkThreads are modular and stackable  
  * ChunkThreadChild \- An abstraction class that only exposes the most commonly modified functions  
* ChunkModifierComponent \- Attach this to your PlayerController. It handles player input and replication functions  
* ChunkActor \- Simple class that represents a chunk  
* VoxelTypesDatabase \- Central location where voxels are defined and stored. Any class can easily access

## **\~Overview\~**

We are so excited to share the InfiniteVoxelTerrainPlugin with you all\! This tool is the culmination of several years of work and study, in order to make the fastest and most powerful voxel engine we could. Because it is quite complicated it may be tricky to modify. We want to provide any assistance we can to help you. A good place to start is to look at the ChunkThreadChild class, which is an abstract class that overrides the functions you are most likely to want to modify first.

**Join Our [Discord](https://discord.gg/VSCfnzhhVG)\!** We are here to support you\!

**This plugin makes heavy use of multithreading,** As such it is important to be aware of which thread your code is running on. Unless otherwise stated in comments, all functions in the ChunkManager run on the game thread (the default in UE), and all functions in ChunkThread will run on an FRunnableThread owned by the given ChunkThread. It is generally unsafe to access shared data between threads, unless you use an FScopeLock. If you are unfamiliar with best practices regarding multithreading, we recommend reading this [Unreal Forum Post](https://forums.unrealengine.com/t/multithreading-and-performance-in-unreal/1216417) that explains how to work with threads.

**This voxel engine is currently geared towards making block worlds,** and as such, there is currently no support for smooth or realistic terrain. That could change in the future, but keep in mind it is not the goal of this plugin

## 

## **\~Getting Started\~**

* **Check out the L\_VoxelTerrainExample world**  
* **Incorporate the terrain generator in your own Level.** Place a BP\_ChunkManager and VoxelTypesDatabase in your world. If you want you can copy them from the example world. Make sure the first element of VoxelTypesDatabase is an empty voxel marked as bIsAir\! (Later this will be mandatory)  
* **Add new voxels** to the VoxelDefinitions in the details panel of VoxelTypesDatabase. Specifying a top texture only will make that texture the side texture as well for the block icon  
* **Check out Auburn’s NoiseTool** Which generates the EncodedNodeTrees this plugin uses to generate the terrain (see [InitializeNoiseGenerators](#~chunkthread~) below). The NoiseTool can be found on GitHub at https://github.com/Auburn/FastNoise2/releases. Find the latest release and click “assets” and download the version matching your operating system. Once the asset is downloaded, extract the folder, then navigate to “bin” and find **NoiseTool.exe**. Please bear in mind that only 2D noise is supported with the InfiniteVoxelTerrainPlugin. We hope to add 3D noise support in the future\!  
* **Change the shape of the generated terrain\!** To do this, create a node tree with the NoiseTool. You can start with an example by right clicking in the node graph and selecting Import-\>SimpleTerrain. Just remember that since this plugin is currently 2D only, you will need to remove the included “Add” and “PositionOutput” nodes right before using your Node Tree. They are useful for visualization of your terrain inside the NoiseTool, but are not needed for the InfiniteVoxelTerrainPlugin. Once you have completed your node tree in the NoiseTool, you can right click the last node in the tree and select “Copy Encoded Node Tree” and past it into one of the strings in **InitializeNoiseGenerators()** like this: PlainsNoiseGenerator \= FastNoise::NewFromEncodedNodeTree("`YOUR_ENCODED_NODE_TREE`");  
  Bear in mind that each NoiseGenerator has a different multiplier applied to it. These can be changed in the switch (BiomePoint.Key) in **GenerateHeightmap().**  
  You can remove these multipliers if you want the terrain to match exactly what you had in the NoiseTool.

## **\~ChunkManager\~** {#~chunkmanager~}

### 

### Purpose:

Sets up ChunkThreads and handles game thread modification of Chunks. Also handles player tracking

### Core Functions:

* **InitializeThreads()** We set up a number of `FChunkThread` instances based on how many cores the PC has, minus the number of threads you want to keep free. If your PC spends too much time on `Game` when profiling with StatUnit, you can change `NumThreadsToKeepFree` to a higher number. You can also increase `ThreadWorkingSleepTime` as another way to pace chunk generation speed  
* **UpdateTrackedLocations()** keeps track of player positions in the game world and manages how chunks are generated and replicated based on where players are. It loops through all tracked players, checking if their current positions have changed. If a player moves into a new chunk, it updates their location and handles any necessary changes, like updating which chunks need to be loaded or replicated, especially on server setups.  
  If a player's starting chunk hasn’t been generated yet, the function temporarily freezes their movement until the chunk is ready, ensuring they don’t move into ungenerated areas. The function also manages which chunks get replicated to clients, especially on servers, and updates the player locations that are used by the `ChunkThreads`. Finally, it cleans up any invalid player references and replicates the updated locations when needed.  
* **UpateNearbyChunkCollisions()** manages collision generation for nearby chunks based on player locations. It uses a spiral method to search outward from each player's current location, identifying chunks that need collision generated. The function first collects all chunk cells within a specified radius and then checks each cell to find the corresponding chunk actors.  
  If a chunk is found and valid, it is added to a list, and if conditions are met (like being on a server), replication for that chunk is enabled. The function then generates collisions for the nearby chunks that haven't already been processed and asynchronously decompresses voxel data if needed. This ensures that collisions are up-to-date around active players, enhancing gameplay performance and synchronization.  
* **HandleClientNeededServerData()** manages the distribution of region data needed by clients from the server. It tries to acquire a lock immediately; if not possible on the game thread, it runs the task on a background thread. If successful, the function iterates through tracked player controllers, checking for regions they need data for. If a region is already loaded, it sends the data to the client; if not, it adds the region to the pending load list. It also removes any invalid player controllers from the tracked list. This function ensures that clients receive necessary data promptly, maintaining game performance and synchronization.  
* **SetVoxel()** Changes the value of a voxel (block) in a chunk. We may have to decompress the voxels in the chunk if they were compressed. If this voxel is on the border of the chunk, we need to modify the adjacent chunk as well. We then call   
  * **UpdateModifiedVoxels()** first identifies the region containing the modified chunk and marks it as changed since the last save. Then, it updates or initializes the voxel modification data within that region. If the chunk already has modified voxel data, it updates the specific voxel's value. If the chunk has no modification data yet, it initializes a new array with a default unmodified state, updating the specified voxel with the new value. This function ensures changes to voxels are correctly tracked and saved.

  We then check to see if we’ve built up or down into a new chunk that hasn’t spawned yet. Because our system only spawns vertical chunks that are needed, we may need to spawn a new one. On an async thread, we call

  * **SpawnAdditionalVerticalChunk()** prepares the construction data for the new chunk and generates the heightmap and voxel data on a background thread. Once the chunk data is ready, it schedules the chunk to be spawned on the game thread and, if running on a server, updates chunk names for replication. This ensures the new chunk is created and synchronized appropriately within the voxel world

* **ReplicateChunkNames()** handles the replication of chunk names to the client. This is necessary because we are linking AActors that are spawned locally on the server and client separately, and we have to be sure we are using a network name that is deterministic, but also hasn’t been used by a previous chunk in this location. To do this, the function first verifies that it is running on a server and retrieves all chunk cells within a specified radius around a center cell. After retrieving the chunk data, it asynchronously processes this data on the game thread, enabling replication for each valid chunk and collects the chunk cell and spawn count information. This data is then passed to   
  * **SendChunkNameDataToClients()** sends the collected chunk name data to all valid clients. For each tracked player controller, it checks if the player is valid and ready for data replication. It then retrieves the `UChunkModifierComponent` from the player controller and compares the chunk cells in the `FChunkNameData` with those already up-to-date. Any chunk cells already up-to-date are removed from the data, and the updated chunk name data is sent to the client using `ClientReceiveChunkNameData`. This function ensures that each client receives the latest chunk information while filtering out redundant data.  
    The client will then use this data to name its chunks with **ClientSetChunkNames()**.

## 

## **\~ChunkThread\~** {#~chunkthread~}

### Purpose:

Handles the generation of Chunks, including noise generation, voxel generation and mesh generation. ChunkThreads are modular and stackable

### Core Functions:

* Run()

  This is where we handle all the chunk generation code.

  Before we begin the while loop, we call 

  * InitializeNoiseGenerators**()** This is where you will want to put your EncodedNodeTrees if you are using Auburn’s NoiseTool. There is some very basic support for biomes here, but it is hard to customize, so this system will hopefully be redone in a future update to make it more modular and easy to use. To do this, create a node tree with the NoiseTool, then right click the last node in the tree and select “Copy Encoded Node Tree” and past it into one of the strings in **InitializeNoiseGenerators()** like this: PlainsNoiseGenerator \= FastNoise::NewFromEncodedNodeTree("`YOUR_ENCODED_NODE_TREE`");

  Next we run the while loop. As this is run on the thread, we have to be very careful with how we access any data that is not owned internally by this instance of ChunkThread. 

  * **UpdateTrackingVariables()** Here we are accessing our ChunkManagerRef to get the player locations.   
    We then use that information to set some other tracking variables such as `bDidTrackedActorMove`, as well as `TrackedChunkRingCount` and `TrackedChunkRingDistance.` These will be used later on in **Run()** to determine which ring of chunks we are generating.  
  * **UpdateTempVariables()** We copy some variables to temporary duplicates as they need to be modifiable anytime on the game thread, and we can’t have them change while we are using them on this worker thread.

  We do some checks to make sure it is safe to continue with chunk generation, then 

  * **UpdateChunks()** (Only runs on the first thread) This function has to do some heavy lifting. It checks all the chunks to determine which ones need to be destroyed, hidden, shown, or unreplicated, it then sends this information to the game thread, which performs those operations using `ChunkManagerRef`  
  * **PrepareRegionForGeneration()** Makes sure that we have the current region loaded or is current on multiplayer data.   
    It also saves newly irrelevant regions if we are not on the client (as the client does no saving or loading)

	Once we know the region we are in, we can

* **FindNextNeededHeightmap()** This is a very efficient function. Based on the current location we are generating chunks for it finds the next needed location for a heightmap (and thus a stack of chunks). Think of the process like an onion, but represented on a 2D grid of points. We start on the inside and generate one ring at a time going further and further out. Each time the function is called, we remember which ring we are on (and this knowledge is shared between ChunkThread instances), so we don’t have to recheck every point in every ring. If the client moves, we push the current ring in to make sure we don’t skip some points. When a ring is completed, we move the current ring out to start checking the next ring. Using this method helps us avoid checking every single location that needs a heightmap.

If we find a needed heightmap, we go ahead and generate a stack of chunks for  
	the heightmap in

* **GenerateChunkData()** This is really a group of functions. We start with  
  * **GenerateHeightmap()** Using Auburn’s FastNoise2 library, we generate a “heightmap”, a 2D grid of noise points. This is how the terrain shape is determined. There is a preliminary implementation of biomes here, but is rather crude. Basically, we generate one heightmap used to determine biome locations. This heightmap is divided into five different values, each representing a different biome like this:

    ![][image2]

     This can be visualized by copying the key: ***IgAAAEBAmpmZPhsAEABxPQo/GwAeABcAAAAAAAAAgD9cj8I+AACAPw0AAwAAAAAAQEAJAADsUbg+AOxRuD4AAAAAAAETAI/CdT7//wEAAOxROD4AAAAAQA==***  
    Into the NoiseTool by right clicking in the graph and clicking Import-\>Encoded Node Tree

    Each value in this heightmap corresponds to a different NoiseGenerator, (with some blending).   
    You can replace any of the keys in **InitializeNoiseGenerators()** with something you created with the NoiseTool.   
    To do this, create a node tree with the NoiseTool, then right click the last node in the tree and select “Copy Encoded Node Tree” and past it into one of the strings in **InitializeNoiseGenerators()** like this: PlainsNoiseGenerator \= FastNoise::NewFromEncodedNodeTree("`YOUR ENCODED NODE TREE`");

    Another thing to note about **GenerateHeightmap()** is that we are using the highest and lowest noise point to determine how many chunks we need on the Z axis (up and down). This is actually the only thing keeping us from using 3D noise. With 2D noise there is a known highest and lowest noise point. With 3D noise this is not so simple. We may come up with a 3D noise solution to generating chunks if there is enough demand for it.

    * **ComineChunkZIndices()** This function takes the `ChunkZIndices` we’ve generated while generating the heightmap and combines them with whatever additional ZIndices will be needed by additional chunks players have modified voxels in.  
      `ChunkZIndices` simply determines which vertical chunks we need to generate in this stack.  
    * **GenerateVoxelsForChunks()** Generates whatever voxels are needed by the default terrain, and then incorporates whatever player modified voxels we’ve got stored using  
      * **GenerateChunkVoxels()** Essentially generates a stack of voxels in each X and Y location in the chunk using the noise point to determine the height. If you want to modify what voxels are generated where, this is the place to do it. Just make sure whatever you add is deterministic and tied to the world seed. (If you don’t add anything new that is random, procedural or noise based this should already be the case)  
      * **ApplyModifiedVoxelsToChunk()** These voxels could exist from changes we made this session, changes from a loaded save, or they could be received from the server. We overwrite whatever **GenerateVoxelsForChunks()** gave us for this chunk, using UINT8\_MAX to represent a non-modified voxel. This is so 0 can still represent an air block, allowing us to store destruction of the terrain.  
    * **GenerateMeshDataForChunks()** we generate the mesh data for each chunk using  
      * **GenerateChunkMeshData()** This can look really daunting at first, but it’s actually fairly simple, and worth trying to understand if you want to change the way the voxels are represented to anything other than a simple block.  
        First we declare a bunch of `TRealtimeMeshStreamBuilder` variables that are used to store data for the mesh the (third party) `RealtimeMeshComponent` should create. Next we  iterate through each voxel in a 3D grid defined by `VoxelCount` on the X, Y, and Z axes, calculating each voxel's world position based on `ChunkMeshOffset` and `VoxelSize`. For each voxel, it checks its value from the `Voxels` array to determine if it's solid (non-air); if `[VoxelValue].bIsAir`, it skips further processing. If solid, the function checks all six adjacent voxels using directional offsets to see if any faces are exposed. If an adjacent voxel is air or transparent, the function generates that face's vertices using predefined offsets (`CubeVertLocations`), calculates normals and tangents, assigns basic colors, and computes UV coordinates for texturing. These faces are grouped by voxel value in `TrianglesByVoxelValue`, with each face represented by two triangles. This method efficiently generates only visible geometry, avoiding unnecessary rendering of buried or hidden faces

      The last function in **GenerateChunkMeshData()** gets called only if the chunk is outside the collision range (and thus we know it cannot be modified)

    * **CompressVoxelData()** uses **RunLengthEncode()** to pack up the voxels  
      * **RunLengthEncode()** This function performs run-length encoding on `VoxelData`, which compresses the voxel information by replacing consecutive repeated values with a count and the value itself. It initializes an `EncodedData` array, reserving half the size of the original data to optimize memory usage. Starting with the first voxel value, it iterates through the array, counting consecutive occurrences of the same value up to a maximum of 255 (`MAX_uint8`). When the value changes or the count limit is reached, it stores the count and value in `EncodedData` and resets for the next sequence. Finally, the encoded data replaces the original `VoxelData`, reducing its size and improving storage efficiency.

  Now that we are back in **Run()**, and have finished generating our chunk data for this heightmap location, we call

  * **AsyncSpawnChunks()** We have to spawn AActors on the game thread, so we loop through the `ChunkConstructionData` for each chunk, running an **AsyncTask** and calling   
    * **SpawnChunkFromConstructionData()** Either spawns a new chunk, or grabs one from the `ChunksToDestroyQueue`, we then set up some parameters. Next, if we are on the client we check `ChunkSpawnCountByCell` which stores the chunk counts we’ve received from the server. The chunk count is used to create a deterministic network name for this actor. (We need a new name every time the actor is spawned on the server, and we can’t destroy it on the client until it’s destroyed on the server if this is a replicated chunk). We use the `ChunkSpawnCount` to set the chunk name. We then access the `ChunkManagerRef` to add this chunk’s Z index, so we can use it later.  
      If we didn’t create a network name for this chunk, it means that it’s currently safe to destroy if we need to later.   
      We finalize spawning, and if this is the server, we call  
      * **EnableReplicationForChunk()** which generates a deterministic network name for this chunk based on how many times a chunk has been spawned in this `ChunkCell`, and enables replication

      We can then calculate the materials needed based on what voxel values are in this chunk using **ChunkManagerRef-\>GetMaterialsForChunkData(),** and incorporate the materials with the `ChunkData`. Then we pass off the `MeshData` to

* **Chunk-\>GenerateChunkMesh()** uses the provided mesh data and materials to create the chunk mesh with the RealtimeMeshComponent. First, it checks if the world, mesh components, and mesh data are valid. If anything is missing or the mesh data is empty, it disables collision, removes any existing mesh sections, and exits. If the data is valid, it enables collision and clears old mesh section keys.   
  Next, it sets up material slots for each voxel section using the materials passed in. It then creates a section group for each voxel value and fills it with the mesh data from ChunkMeshData. Each section is configured based on whether collision should be generated, depending on the mesh data settings. Finally, the function flags that mesh generation is complete, ensuring the chunk is fully set up and ready for use in the game world.

  At this point we have located, populated and generated a whole stack of chunks for a needed heightmap location (2D point on the chunk grid) and are ready to begin the **Run()** function all over again for the next chunk.

